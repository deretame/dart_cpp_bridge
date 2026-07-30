---
title: 外部运行时集成
description: 其他事件循环（libuv、glib 等）如何接入主调度器并调用 Dart 回调函数
---

本页说明如何让**非 asio 事件循环**（libuv、glib、自定义 loop 等）接入 bridge 的协程系统，并从外部运行时调用 Dart 回调函数（DartFn）。

## 背景

bridge 的核心调度基于 `asio::io_context` + `AsioExecutor`。但实际项目中，C++ 侧可能已有自己的事件循环（如 libuv），需要：

1. 与 bridge 的 channel/coroutine 系统通信
2. 调用 Dart 侧注册的回调函数

接入方式：写一个 **Worker 类**包装你的事件循环，实现一个固定模式的 schedule 回调（加锁入队 → 唤醒 loop → drain 执行），然后注册到 bridge。bridge 内部会创建一个 `ForeignExecutor`（`async_simple::Executor` 实现），之后 channel / 协程就能透明地调度到你的 loop 线程上。

## 架构

```text
┌────────────────────────────────────────────────────┐
│  你写的 Worker 类（包装外部事件循环）              │
│  • 任务队列 (mutex + queue)                      │
│  • 唤醒机制 (uv_async_send / eventfd / ...)     │
│  • drain 回调（在 loop 线程执行 fn(userdata)）   │
│  • 线程管理 (start / stop)                       │
└──────────────────────┬─────────────────────────────┘
                       │ dcb_foreign_register(name, schedule_fn, ctx)
                       ▼
┌────────────────────────────────────────────────────┐
│  ForeignExecutor (bridge 内部创建)                  │
│  schedule(Func) → 装箱到堆 → 调用你的 schedule_fn │
└──────────────────────┬─────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────┐
│  bridge channel 系统 (co::oneshot / co::mpsc)       │
│  send() → wake_waiter → executor->schedule(resume) │
└────────────────────────────────────────────────────┘
```

你需要写的就是最上面那一层。下面的 ForeignExecutor 和 channel 都是 bridge 已有的。

## 接入步骤

### Worker 的契约

你的 Worker 类只需满足一个约定：**实现一个 schedule 回调，保证 `fn(userdata)` 在 loop 线程上执行**。

```c
// bridge 调用此回调投递任务，你必须保证 fn(userdata) 最终在 loop 线程上被调用
typedef void (*dcb_schedule_fn)(void (*fn)(void*), void* userdata, void* ctx);
//                                            │              │           │
//                                   要执行的函数    fn 的参数     你注册时传的 ctx
//                                  （协程恢复等）  （堆上，fn 内部释放）  （如 UvWorker*）
```

要求：
- **线程安全**：bridge 可能从任意线程调用此回调，入队必须加锁
- **不阻塞**：回调本身只做入队 + 唤醒，不能同步执行 fn
- **必须执行**：每个 `fn(userdata)` 都必须被调用一次（否则协程泄漏）

### bridge 提供的 C API

`#include "dart_cpp_bridge/foreign_runtime.h"`：

| 函数 | 作用 | 调用时机 |
|------|------|----------|
| `dcb_foreign_register(name, schedule_fn, ctx)` | 注册运行时，返回 runtime_id | Worker 启动时 |
| `dcb_foreign_mark_loop_thread(id)` | 标记当前线程为 loop 线程 | loop 线程启动后、跑协程前 |
| `dcb_foreign_executor(id)` | 获取 ForeignExecutor 指针 | 需要 `.via(ex)` 或 channel 时 |
| `dcb_foreign_unregister(id)` | 注销（之后不再收到任务） | Worker 停止时 |
| `dcb_post_to_bridge(fn, userdata)` | 向 bridge io 线程投递任务 | 外部处理完要通知 bridge 时 |

### 示例：完整的 UvWorker

```cpp
#include <uv.h>
#include <mutex>
#include <queue>
#include <thread>
#include "dart_cpp_bridge/foreign_runtime.h"  // dcb_foreign_register 等 C API
#include "dart_cpp_bridge/foreign_executor.hpp"  // dcb::ForeignExecutor

class UvWorker {
 public:
  void start() {
    uv_loop_init(&loop_);

    // async handle：跨线程唤醒 loop 线程
    uv_async_init(&loop_, &async_, [](uv_async_t* h) {
      auto* self = static_cast<UvWorker*>(h->data);
      self->drain();  // 在 loop 线程上执行所有待处理任务
    });
    async_.data = this;

    // 注册到 bridge，获取 runtime ID
    id_ = dcb_foreign_register("my-worker", &schedule_cb, this);

    // 启动 loop 线程
    thread_ = std::thread([this] {
      dcb_foreign_mark_loop_thread(id_);  // 标记当前线程为 loop 线程
      uv_run(&loop_, UV_RUN_DEFAULT);
    });
  }

  void stop() {
    dcb_foreign_unregister(id_);  // bridge 不再向我们投递任务
    uv_stop(&loop_);
    uv_async_send(&async_);       // 唤醒使其退出 uv_run
    thread_.join();
    uv_loop_close(&loop_);
  }

  // 获取 bridge 为此运行时创建的 ForeignExecutor（用于 .via(ex) / channel coAwait）
  dcb::ForeignExecutor* executor() {
    return static_cast<dcb::ForeignExecutor*>(dcb_foreign_executor(id_));
  }

 private:
  // ─── bridge 调用此函数投递任务（可能从任意线程调用） ───
  static void schedule_cb(void (*fn)(void*), void* userdata, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    {
      std::lock_guard lock(self->mu_);
      self->pending_.push({fn, userdata});  // 加锁入队
    }
    uv_async_send(&self->async_);  // 线程安全地唤醒 loop
  }

  // ─── 在 loop 线程上执行所有待处理任务 ───
  void drain() {
    std::queue<std::pair<void (*)(void*), void*>> batch;
    {
      std::lock_guard lock(mu_);
      batch.swap(pending_);  // 一次性取出，减少持锁时间
    }
    while (!batch.empty()) {
      auto [fn, ud] = batch.front();
      batch.pop();
      fn(ud);  // 执行（如协程恢复、channel wakeup 等）
    }
  }

  uv_loop_t loop_{};
  uv_async_t async_{};
  std::mutex mu_;
  std::queue<std::pair<void (*)(void*), void*>> pending_;
  std::thread thread_;
  uint32_t id_{0};
};
```

:::note
`dcb_foreign_executor()` 返回的是 `void*`，实际类型为 `dcb::ForeignExecutor*`，使用时要 `static_cast` 转换。示例中的 `executor()` 方法已经帮你做了这件事。
:::

### 工作原理

```text
bridge 任意线程                    UvWorker                     libuv loop 线程
───────────────────────────────────────────────────────────────────────────
ForeignExecutor::schedule(func)
  → 装箱 func 到堆
  → 调用 schedule_cb(fn, ud, ctx)
                              lock + push {fn, ud}
                              uv_async_send()
                                                      ──────▶  async 回调触发
                                                               drain():
                                                                 fn(ud)
                                                                 → 执行 func
                                                                 → 协程恢复 / channel wakeup
```

关键点：
- `schedule_cb` 可以从**任意线程**被调用（bridge io 线程、其他 worker 等），所以入队必须加锁
- `uv_async_send` 是 libuv 唯一线程安全的唤醒方式
- `drain()` 始终在 loop 线程上执行，所以 `fn(ud)` 无需额外同步

### 其他事件循环

| 运行时 | 替代 `uv_async_send` | 替代 `drain` 触发点 |
|---------|---------------------|--------------------|
| glib | `g_idle_add()` 或 `g_async_queue_push()` | idle callback |
| epoll 自研 | `eventfd` + write | epoll_wait 返回后 |
| Windows | `PostMessage()` | WndProc |

模式都一样：**加锁入队 → 唤醒 loop → loop 线程 drain 执行**。

## 从外部运行时调用 Dart 回调 (DartFn)

### 方式一：非阻塞（推荐）

等待 Dart 回复期间 **不阻塞** loop 线程。协程挂起后，Dart 回复时自动在 loop 线程上恢复。

```cpp
// 全局 Worker（在应用启动时 start()，退出时 stop()）
static UvWorker g_worker;

// static 协程函数（MSVC 下必须用此模式，见下方注意事项）
static async_simple::coro::Lazy<> my_dart_fn_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  try {
    auto result = co_await cb(input);  // 非阻塞：挂起直到 Dart 回复
    tx_ptr->send(std::move(result));
  } catch (const std::exception& e) {
    tx_ptr->send(std::string("ERROR: ") + e.what());
  }
  co_return;
}

// API 函数（运行在 bridge io 线程）：
async_simple::coro::Lazy<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  auto* ex = g_worker.executor();  // 获取 ForeignExecutor
  auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

  // 在外部 loop 线程上启动协程
  ex->schedule([tx_ptr, cb = std::move(callback), input = std::move(input), ex]() mutable {
    my_dart_fn_coro(std::move(tx_ptr), std::move(cb), std::move(input))
        .via(ex)
        .start([](auto&&) {});
  });

  // bridge 侧挂起等待结果（不阻塞 io 线程）
  auto reply = co_await rx.recv();
  if (!reply) throw std::runtime_error("worker dropped");
  co_return *reply;
}
```

**工作原理：**

1. `ex->schedule(...)` 将协程启动投递到 loop 线程
2. `.via(ex).start()` 将协程绑定到 ForeignExecutor 并开始执行
3. `co_await cb(input)` 内部：
   - 创建 oneshot channel
   - 编码参数，发送 DartFnCall 帧到 Dart
   - `co_await rx.recv()` 挂起协程，channel 自动捕获 ForeignExecutor
4. Dart 执行回调后回复 → `complete_dart_fn` → `tx.send()`
5. `wake_waiter(h, ex)` → `ex->schedule(resume)` → 协程在 loop 线程恢复
6. 结果通过外层 oneshot channel 传回 bridge 主运行时

### 方式二：阻塞（更简单）

阻塞当前线程直到 Dart 回复。可在**任何线程**使用（bridge io 线程除外）。无需 ForeignExecutor 配置。

```cpp
// 在任意工作线程上（不能是 bridge io 线程！）
auto result = async_simple::coro::syncAwait(dcb::spawn(cb(input)));
```

:::caution[注意]
- 在 loop 线程上使用 `syncAwait` 会阻塞整个事件循环直到 Dart 响应
- 在 bridge io 线程上使用会**自死锁**（Dart 回复需要 io 线程分发）
:::

### 对比

| | 非阻塞 | 阻塞 |
|--|---|---|
| 是否卡 loop | 否 | 是（直到 Dart 回复） |
| ForeignExecutor 配置 | 完整（虚函数 + mark_loop_thread） | 无 |
| MSVC workaround | 需要（static 协程函数） | 不需要 |
| 适用场景 | 生产环境、并发工作 | 快速测试、简单脚本 |

## 跨运行时通信：channel 服务模式

上面的“方式一”是一次性交互（发一个任务，等一个结果）。如果运行时 A 需要**长期运行**并接收多个运行时 B/C/D 的请求，可以用 mpsc channel 暴露一个“服务”：

```text
运行时 B ───┐
              │  tx.send(请求 + 回复通道)
运行时 C ───┼──────────────▶  运行时 A（mpsc receiver 循环）
              │                    │
运行时 D ───┘                    │ 处理完 → reply_tx.send(结果)
                                     ▼
                              B/C/D 的 oneshot rx 收到结果
```

### 示例：A 暴露服务，B 调用

```cpp
#include "dart_cpp_bridge/channel.hpp"

// ─── 请求类型：数据 + 一次性回复通道 ───
struct Request {
  std::string payload;                              // 任务数据
  co::oneshot::Sender<std::string> reply_tx;        // A 处理完后通过此通道回复
};

// ─── 运行时 A：长期运行的服务循环 ───
// A 暴露 sender 给其他运行时，自己持有 receiver 循环处理
class ServiceA {
 public:
  // 其他运行时通过此 sender 发送请求
  co::mpsc::Sender<Request> sender() { return tx_; }

  // 在 A 的 loop 线程上启动服务循环
  void run(dcb::ForeignExecutor* ex) {
    service_loop(std::move(rx_)).via(ex).start([](auto&&) {});
  }

 private:
  static async_simple::coro::Lazy<> service_loop(co::mpsc::Receiver<Request> rx) {
    while (auto req = co_await rx.recv()) {  // 挂起等待下一个请求
      // 处理任务...
      std::string result = "processed: " + req->payload;
      // 通过 B 给的一次性通道回复
      req->reply_tx.send(std::move(result));
    }
    co_return;  // channel 关闭，服务结束
  }

  co::mpsc::Sender<Request> tx_;
  co::mpsc::Receiver<Request> rx_;

 public:
  ServiceA() { auto [tx, rx] = co::mpsc::unbounded<Request>(); tx_ = std::move(tx); rx_ = std::move(rx); }
};

// ─── 运行时 B：发送请求并非阻塞等待结果 ───
async_simple::coro::Lazy<std::string> call_service_a(
    co::mpsc::Sender<Request> a_sender, std::string data) {
  // 创建一次性回复通道
  auto [reply_tx, reply_rx] = co::oneshot::channel<std::string>();

  // 发送请求到 A（非阻塞，任意线程可调）
  a_sender.send(Request{std::move(data), std::move(reply_tx)});

  // 非阻塞等待 A 的回复（协程挂起，不占线程）
  auto result = co_await reply_rx.recv();
  if (!result) throw std::runtime_error("service A dropped");
  co_return *result;
}
```

:::note[实现细节]
- `Request` 因为包含 `co::oneshot::Sender` 是 **move-only** 的，`mpsc::send(Request{...})` 会移动它，不能拷贝
- `ServiceA::run()` 只能调用一次：`std::move(rx_)` 之后 `rx_` 就空了，重复调用会启动一个立刻看到 channel 关闭的循环
- 如果 `Request` 本身不能满足底层 `moodycamel::ConcurrentQueue` 的约束，可以把它包在 `std::shared_ptr<Request>` 或 `std::unique_ptr<Request>` 里再入队
:::

### 与“方式一”的区别

| | 方式一（schedule 协程） | channel 服务模式 |
|--|---|---|
| 交互次数 | 一次性（发一个任务，等一个结果） | 长期（A 持续接收多个请求） |
| 调用方 | 必须知道 A 的 executor | 只需 A 的 sender（线程安全，任意线程可发） |
| 多个调用方 | 每次都要 schedule | B/C/D 共用同一个 sender |
| 适用场景 | 单次跨运行时任务 | 微服务/actor 模式、任务队列 |

核心原理：`tx.send()` 是线程安全的非阻塞操作，内部通过 `wake_waiter` 将 A 的协程恢复调度回 A 的 executor。B 的 `co_await reply_rx.recv()` 同理，回复时自动调度回 B 的 executor。不同运行时之间不需要知道对方的线程模型，channel 透明处理了跨线程调度。

## 使用独立 AsioExecutor 运行时

如果你的"外部运行时"也是基于 asio 的（独立 `io_context` + `AsioExecutor` + 线程），则不需要自己实现 `schedule` 回调——`AsioExecutor` 已经完整实现了 `async_simple::Executor` 的所有虚函数。你可以直接在协程中 `co_await` DartFn：

```cpp
// WorkerRuntime 拥有独立的 io_context + AsioExecutor + thread
// 注意：在 MSVC 上，协程 lambda 同样会触发下方 §MSVC 注意事项 的捕获 bug；
// 生产代码建议写成 static 协程函数，这里为简洁仍用 lambda。
worker->spawn([cb = std::move(dartFn), input]() mutable -> async_simple::coro::Lazy<> {
  auto result = co_await cb(input);  // 直接 co_await，无需额外配置
  // 使用 result...
  co_return;
});
```

详见 `examples/multi_runtime_demo`。

## MSVC 注意事项

:::danger[MSVC 19.51 协程 lambda 捕获 bug]
在 MSVC 上，**协程 lambda** 中捕获的变量（`std::string`、`DartFn`、`shared_ptr` 等）在协程恢复后会变成垃圾值，导致 ACCESS_VIOLATION 崩溃。

**必须使用 static 协程函数 + 参数传递**，不能用协程 lambda：

```cpp
// ✗ 崩溃
auto lazy = [cb, input]() -> Lazy<> {
  co_await cb(input);  // cb/input 已损坏！
};

// ✓ 正确
static Lazy<> my_coro(DartFn<...> cb, std::string input) {
  co_await cb(input);  // 参数完好
}
```
:::

此 bug 与 ForeignExecutor 无关，在任何 executor 上的协程 lambda 都可能触发。详见 [async-simple 协程入门](/dart_cpp_bridge/guides/fundamentals/async-simple/) 和 [已知问题 §10](/dart_cpp_bridge/reference/known-issues/)。

## 关键设计约束

| 约束 | 说明 |
|------|------|
| schedule 必须线程安全 | bridge 可能从任意线程调用 |
| fn(userdata) 必须在 loop 线程执行 | 协程正确恢复的前提 |
| 不要阻塞 loop 线程 | sleep / 同步 IO 会卡死整个事件循环 |
| std::function 要求可拷贝 | 捕获 move-only 类型时用 `shared_ptr` 包装 |
| 注销后不再收到 schedule | `dcb_foreign_unregister` 后 executor 失效 |
| executor 失效时 fallback | channel 的 `wake_waiter` 会 inline resume（防协程泄漏） |

## 完整示例

- **libuv + ForeignExecutor**: `examples/foreign_runtime_demo`
- **独立 AsioExecutor 运行时**: `examples/multi_runtime_demo`

## 不想用 C++ 协程？

如果你的代码是纯 C，或者不想引入 async-simple / asio 依赖，可以使用 [纯 C 桥接 API](/dart_cpp_bridge/guides/advanced/cbridge/)——零依赖的 callback 风格接口，从任意线程调用 Dart 函数或等待外部异步操作。
