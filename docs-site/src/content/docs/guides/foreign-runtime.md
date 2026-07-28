---
title: 外部运行时集成
description: 其他事件循环（libuv、glib 等）如何接入主调度器并调用 Dart 回调函数
---

本页详细说明如何让**非 asio 事件循环**（libuv、glib、自定义 loop 等）接入 bridge 的协程系统，并从外部运行时调用已注册的 Dart 回调函数（DartFn）。

## 背景

bridge 的核心调度基于 `asio::io_context` + `AsioExecutor`。但实际项目中，C++ 侧可能已有自己的事件循环（如 libuv），需要：

1. 与 bridge 的 channel/coroutine 系统通信
2. 调用 Dart 侧注册的回调函数

`ForeignExecutor` 适配层让任何事件循环只需实现 **"在 loop 线程上执行一个 `void(*)(void*)`"** 即可接入。

## 架构

```text
┌────────────────────────────────────────────────────┐
│  外部运行时（libuv / glib / 自定义 loop）           │
│  只需实现：在 loop 线程上执行 void(*)(void*)        │
└──────────────────────┬─────────────────────────────┘
                       │ dcb_foreign_register(name, schedule_fn, ctx)
                       ▼
┌────────────────────────────────────────────────────┐
│  ForeignExecutor (async_simple::Executor 实现)      │
│  schedule(Func) → 装箱到堆 → 调用 schedule_fn      │
└──────────────────────┬─────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────┐
│  bridge channel 系统 (co::oneshot / co::mpsc)       │
│  send() → wake_waiter → executor->schedule(resume) │
└────────────────────────────────────────────────────┘
```

## 接入步骤

### 1. 实现 schedule 回调

你的事件循环只需提供一个函数，将 `void(*)(void*)` 投递到 loop 线程执行：

```cpp
// libuv 示例
static void schedule_callback(void (*fn)(void*), void* userdata, void* ctx) {
  auto* self = static_cast<UvWorker*>(ctx);
  self->push_task({fn, userdata});   // 加锁入队
  uv_async_send(&self->async_);      // 唤醒 loop
}
```

### 2. 注册到 bridge

```cpp
uint32_t id = dcb_foreign_register("my-worker", &schedule_callback, this);
```

### 3. 标记 loop 线程

在 loop 线程启动后、执行任何协程之前调用：

```cpp
// 在 loop 线程的入口处
dcb_foreign_mark_loop_thread(id);
```

这使 `ForeignExecutor::currentThreadInExecutor()` 能正确判断当前线程。

### 4. 获取 Executor 指针

```cpp
auto* ex = static_cast<dcb::ForeignExecutor*>(dcb_foreign_executor(id));
```

## 从外部运行时调用 Dart 回调 (DartFn)

### 方式一：非阻塞（推荐）

等待 Dart 回复期间 **不阻塞** loop 线程。协程挂起后，Dart 回复时自动在 loop 线程上恢复。

```cpp
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
  auto* ex = worker->executor();
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

## 使用独立 AsioExecutor 运行时

如果你的"外部运行时"也是基于 asio 的（独立 `io_context` + `AsioExecutor` + 线程），则更简单——直接在协程中 `co_await` DartFn 即可：

```cpp
// WorkerRuntime 拥有独立的 io_context + AsioExecutor + thread
worker->spawn([cb = std::move(dartFn), input]() mutable -> Lazy<> {
  auto result = co_await cb(input);  // 直接 co_await，无需额外配置
  // 使用 result...
  co_return;
});
```

AsioExecutor 已完整实现所有虚函数，无需任何 workaround。详见 `examples/multi_runtime_demo`。

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

此 bug 与 ForeignExecutor 无关，在任何 executor 上的协程 lambda 都可能触发。详见 [已知问题 §10](/reference/known-issues/)。

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
