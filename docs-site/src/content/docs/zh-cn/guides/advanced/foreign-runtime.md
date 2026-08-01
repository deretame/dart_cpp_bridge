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

可选地，Worker 还可以注册**原生定时器回调**，让
`co_await async_simple::coro::sleep(...)` 使用事件循环自己的定时器，而不是
每 sleep 开一个等待线程（详见下文“在 ForeignExecutor 上 sleep 与取消”）：

```c
// 在 delay_us 微秒后，于 loop 线程上执行 fn(userdata)。
// 返回不透明 timer 句柄；失败返回 NULL（bridge 回退到等待线程；
// 失败时保证不会调用 fn，也不能使用 userdata）。
typedef void* (*dcb_schedule_after_fn)(
    void (*fn)(void*), void* userdata, int64_t delay_us, void* ctx);

// 取消挂起的定时器。必须线程安全；对已触发或未知句柄必须是安全 no-op。
typedef void (*dcb_cancel_after_fn)(void* timer_handle, void* ctx);
```

### bridge 提供的 C API

`#include "dart_cpp_bridge/foreign_runtime.h"`：

| 函数 | 作用 | 调用时机 |
|------|------|----------|
| `dcb_foreign_register(name, schedule_fn, ctx)` | 注册运行时，返回 runtime_id | Worker 启动时 |
| `dcb_foreign_register_ex(name, schedule_fn, schedule_after_fn, cancel_after_fn, ctx)` | 注册并可选携带原生定时器支持（传 NULL, NULL 与 `dcb_foreign_register` 行为一致） | Worker 启动时 |
| `dcb_foreign_mark_loop_thread(id)` | 标记当前线程为 loop 线程 | loop 线程启动后、跑协程前 |
| `dcb_foreign_executor(id)` | 获取 ForeignExecutor 指针 | 需要 `.via(ex)` 或 channel 时 |
| `dcb_foreign_unregister(id)` | 注销（之后不再收到任务） | Worker 停止时 |
| `dcb_post_to_bridge(fn, userdata)` | 向 bridge io 线程投递任务 | 外部处理完要通知 bridge 时 |

### 示例：完整的 UvWorker

```cpp
#include <uv.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include "dart_cpp_bridge/foreign_runtime.h"  // dcb_foreign_register_ex 等 C API
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

    // 注册到 bridge，并启用原生定时器：
    // 这样 co_await sleep() 使用 uv_timer_t，而不是每 sleep 开一个等待线程。
    id_ = dcb_foreign_register_ex(
        "my-worker", &schedule_cb, &schedule_after_cb, &cancel_after_cb, this);

    // 启动 loop 线程
    thread_ = std::thread([this] {
      dcb_foreign_mark_loop_thread(id_);  // 标记当前线程为 loop 线程
      uv_run(&loop_, UV_RUN_DEFAULT);
    });
  }

  void stop() {
    dcb_foreign_unregister(id_);  // bridge 不再向我们投递任务

    // 在 uv_run 退出前，让 loop 线程清掉所有挂起的定时器
    // （uv_loop_close 在还有存活句柄时会失败）。这里要等清理完成：
    // uv_stop 可能让 uv_run 在异步唤醒被处理之前就退出。
    {
      std::lock_guard lock(mu_);
      if (!live_timers_.empty()) {
        pending_.push({&stop_all_timers_task, this});
        stop_all_requested_.store(true, std::memory_order_release);
      }
    }
    if (stop_all_requested_.load(std::memory_order_acquire)) {
      uv_async_send(&async_);
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!stop_all_done_.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
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

  // ─── 原生定时器支持（可选） ─────────────────────────────────────────────
  //
  // schedule_after_cb 在 loop 线程上被调用（等待中的协程就在那里跑），所以
  // uv_timer_init 是安全的。cancel_after_cb 可能被任意线程调用，它只入队一个
  // 停止任务、绝不直接在 loop 线程之外碰句柄。live_timers_（由 mu_ 保护）
  // 让“定时器已触发/未知句柄”的取消变成安全 no-op。

  struct TimerBox {
    void (*fn)(void*);
    void* userdata;
    UvWorker* self;
  };
  struct CancelTask {
    UvWorker* self;
    uv_timer_t* timer;
  };

  static void* schedule_after_cb(void (*fn)(void*), void* userdata,
                                 int64_t delay_us, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    auto* timer = new uv_timer_t;
    if (uv_timer_init(&self->loop_, timer) != 0) {
      delete timer;
      return nullptr;  // 失败 → bridge 回退到等待线程
    }
    auto* box = new TimerBox{fn, userdata, self};
    timer->data = box;
    const uint64_t timeout_ms =
        delay_us <= 0 ? 1 : static_cast<uint64_t>((delay_us + 999) / 1000);
    if (uv_timer_start(timer, &timer_cb, timeout_ms, 0) != 0) {
      self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
      uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
      return nullptr;
    }
    {
      std::lock_guard lock(self->mu_);
      self->live_timers_.insert(timer);
    }
    return timer;  // 不透明句柄交回给 ForeignExecutor
  }

  static void timer_cb(uv_timer_t* timer) {
    auto* box = static_cast<TimerBox*>(timer->data);
    auto* self = box->self;
    {
      std::lock_guard lock(self->mu_);
      self->live_timers_.erase(timer);
    }
    box->fn(box->userdata);  // 恢复睡眠中的协程
    // 永远不要直接 delete libuv 句柄：它还在 loop 的 handle 队列里，
    // 必须 uv_close，由 close 回调释放。
    self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
  }

  static void timer_close_cb(uv_handle_t* handle) {
    auto* timer = reinterpret_cast<uv_timer_t*>(handle);
    auto* box = static_cast<TimerBox*>(timer->data);
    auto* self = box->self;
    delete box;
    delete timer;
    const int remaining =
        self->pending_closes_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0 &&
        self->stop_all_requested_.load(std::memory_order_acquire)) {
      self->stop_all_done_.store(true, std::memory_order_release);
    }
  }

  static void cancel_after_cb(void* timer_handle, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    auto* timer = static_cast<uv_timer_t*>(timer_handle);
    {
      std::lock_guard lock(self->mu_);
      if (self->live_timers_.find(timer) == self->live_timers_.end()) {
        return;  // 已触发或未知句柄：安全 no-op
      }
      self->pending_.push({&cancel_timer_task, new CancelTask{self, timer}});
    }
    uv_async_send(&self->async_);  // libuv 句柄只在 loop 线程上操作
  }

  static void cancel_timer_task(void* p) {
    auto task =
        std::unique_ptr<CancelTask>(static_cast<CancelTask*>(p));
    auto* self = task->self;
    auto* timer = task->timer;
    {
      std::lock_guard lock(self->mu_);
      if (self->live_timers_.erase(timer) == 0) {
        return;  // timer 回调已经触发并关闭了它
      }
    }
    uv_timer_stop(timer);
    self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
  }

  static void stop_all_timers_task(void* p) {
    auto* self = static_cast<UvWorker*>(p);
    std::vector<uv_timer_t*> timers;
    {
      std::lock_guard lock(self->mu_);
      timers.assign(self->live_timers_.begin(), self->live_timers_.end());
      self->live_timers_.clear();
    }
    for (auto* timer : timers) {
      uv_timer_stop(timer);
      self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
      uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
    }
    // 所有句柄关闭后，由 timer_close_cb 设置 stop_all_done_。
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
  std::unordered_set<uv_timer_t*> live_timers_;
  std::thread thread_;
  uint32_t id_{0};
  std::atomic<int> pending_closes_{0};
  std::atomic<bool> stop_all_requested_{false};
  std::atomic<bool> stop_all_done_{false};
};
```

:::note
`dcb_foreign_executor()` 返回的是 `void*`，实际类型为 `dcb::ForeignExecutor*`，使用时要 `static_cast` 转换。示例中的 `executor()` 方法已经帮你做了这件事。

如果你的事件循环没有定时器能力，直接传 `NULL, NULL`（或者继续用
`dcb_foreign_register`）即可；`co_await sleep()` 会回退到等待线程，取消仍然
可用。
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

### 定时器流程（co_await sleep）

注册了 `schedule_after_cb` / `cancel_after_cb` 之后，`co_await sleep()` 由
`uv_timer_t` 驱动：

```text
等待中的协程（loop 线程）               UvWorker                     libuv loop 线程
───────────────────────────────────────────────────────────────────────────
co_await sleep(dur)
  ForeignExecutor::schedule(4 参)
    → schedule_after_cb(delay_us)
                              new uv_timer_t + uv_timer_start
                              插入 live_timers_
                              返回句柄
    → 在 Slot 上注册 Terminate 处理器
                                                      ──────▶  定时器触发
                                                               timer_cb:
                                                                 移出 live_timers_
                                                                 fn(ud) → 恢复协程
                                                                 uv_close(句柄)

取消路径（任意线程）：
  signal->emits(Terminate)
    → Terminate 处理器
      → cancel_after_cb(句柄)
                              lock；在 live_timers_ 中查找
                              入队 cancel 任务 + uv_async_send
      → schedule_cb(resume)   ← 处理器同时投递协程恢复
                                                      ──────▶  cancel_timer_task:
                                                               uv_timer_stop + uv_close
                                                               resume 任务：
                                                               TimeAwaiter 抛 SignalException
```

定时器回调的关键点：
- `schedule_after_cb` 在 loop 线程上执行（等待中的协程就在那里跑），初始化
  句柄是安全的；返回 `NULL` 表示回退到等待线程。
- `cancel_after_cb` 可能在**任意线程**执行：绝不要直接碰 libuv 句柄——入队
  一个停止任务并唤醒 loop。`live_timers_` 集合让“已触发/未知句柄”的取消成为
  安全 no-op。
- libuv 句柄必须用 `uv_close` 释放（在 close 回调里 free），不能直接
  `delete`。
- 关闭时要在 `uv_stop` **之前**于 loop 线程上关闭所有存活定时器，这样
  `uv_loop_close` 才能成功。

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

## 在 ForeignExecutor 上 sleep 与取消

`ForeignExecutor` 重写了 `schedule(Func, Duration, Slot*)`，在两种实现之间选择：

- **原生定时器（首选）**：如果运行时通过 `dcb_foreign_register_ex` 注册了
  `schedule_after_fn` / `cancel_after_fn`，sleep 由事件循环自己的定时器驱动。
  `SignalType::Terminate` 处理器会停掉定时器并把协程恢复投递回 loop，取消
  及时，且每个 sleep 不再消耗一个线程。
- **等待线程回退**：没有注册定时器回调（或原生定时器启动失败）时，一个分离
  线程在 promise 上等待，直到时长结束**或** `Terminate` 信号触发，然后通过
  `schedule_fn` 投递协程恢复。线程持有共享状态而不是 `this`，executor 被
  注销/销毁后挂起的 sleep 不会再碰它；注销后等待线程直接退出、不再投递。

两条路径的取消行为都和 `AsioExecutor` 一样：用 `Lazy::setLazyLocal` 绑定信号
（或者依赖 `collectAll<Terminate>` / `collectAny<Terminate>`），发出
`SignalType::Terminate`，sleep 就会抛 `async_simple::SignalException`。

两条路径都不会阻塞 loop 线程。“不要阻塞 loop 线程”的约束仍然适用于直接跑在
loop 回调里的**同步**代码（比如 `schedule` lambda 里写
`std::this_thread::sleep_for`），那依然会卡死事件循环。

libuv demo（`examples/codegen_demo`）通过 `dcb_foreign_register_ex` 注册了
原生定时器，`test_foreign_sleep` / `test_foreign_sleep_cancel` 覆盖了 sleep
和取消两条路径。

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
| 不要阻塞 loop 线程 | 在 loop 回调里做**同步** sleep / IO 会卡死事件循环；`co_await coro::sleep(...)` 没问题（原生定时器或等待线程） |
| std::function 要求可拷贝 | 捕获 move-only 类型时用 `shared_ptr` 包装 |
| 注销后不再收到 schedule | `dcb_foreign_unregister` 后 executor 失效 |
| executor 失效时 fallback | channel 的 `wake_waiter` 会 inline resume（防协程泄漏） |

## 完整示例

- **libuv + ForeignExecutor**: `examples/foreign_runtime_demo`
- **独立 AsioExecutor 运行时**: `examples/multi_runtime_demo`

## 不想用 C++ 协程？

如果你的代码是纯 C，或者不想引入 async-simple / asio 依赖，可以使用 [纯 C 桥接 API](/dart_cpp_bridge/guides/advanced/cbridge/)——零依赖的 callback 风格接口，从任意线程调用 Dart 函数或等待外部异步操作。
