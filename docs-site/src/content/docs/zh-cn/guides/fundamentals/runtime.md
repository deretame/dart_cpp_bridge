---
title: 基础运行时
description: dart_cpp_bridge 内置的 asio + async-simple 运行时，以及 spawn、spawn_blocking、channel、sleep 等基础工具
---

`dart_cpp_bridge` 在 C++ 侧自带一个**基于 asio + async-simple** 的运行时。默认情况下，Dart 侧调用 `DartCppBridge.init()` 时会自动启动；业务代码不需要自己创建 `io_context` 或 `Executor`，就能直接写 `async_simple::coro::Lazy<T>` 协程。这章介绍基础工具以及怎么直接使用它们。

## 可以直接使用的库

bridge 通过 FetchContent 已经帮你引入并初始化好了：

- **Asio standalone** — `asio::io_context` 单线程事件循环 + `asio::thread_pool` 阻塞线程池
- **async-simple** — `async_simple::coro::Lazy<T>` 协程、`async_simple::Executor` 调度模型
- **moodycamel::ConcurrentQueue** — `co::mpsc::unbounded<T>` 的底层无锁队列（`concurrentqueue.h`）

常用头文件：

```cpp
#include "dart_cpp_bridge/runtime.hpp"        // Runtime、spawn、spawn_blocking
#include "dart_cpp_bridge/asio_executor.hpp"  // AsioExecutor
#include "dart_cpp_bridge/channel.hpp"        // co::oneshot / co::mpsc
#include "async_simple/coro/Lazy.h"             // Lazy<T>
#include "async_simple/coro/Sleep.h"            // sleep
```

## Runtime 单例

`dcb::Runtime` 是进程级单例，内部持有：

- `asio::io_context` — 单线程事件循环（io 线程）
- `dcb::AsioExecutor` — async-simple 的 `Executor` 实现，把协程调度回 `io_context`
- `asio::thread_pool` — 阻塞工作池（默认 4 线程，可调用 `set_pool_threads()` 调整）

```cpp
#include "dart_cpp_bridge/runtime.hpp"

// 手动启动/停止（通常 Dart 侧 init 时自动完成；C++ 单测需要手动调用）
dcb::Runtime::instance().start();
dcb::Runtime::instance().stop();
```

主要接口：

| 接口 | 作用 |
|---|---|
| `start()` / `stop()` | 启动/停止 io 线程和线程池 |
| `running()` | 是否已启动 |
| `io()` | 获取 `asio::io_context&` |
| `pool()` | 获取 `asio::thread_pool&` |
| `executor()` | 获取 `dcb::AsioExecutor*` |
| `spawn_on_asio(factory)` | 从非协程上下文把一个 Lazy 工厂投递到 io 线程启动 |

## 在业务协程中直接 co_await

由 wire dispatch 调用的业务函数本身就是运行在 io 线程的 `Lazy<T>`，可以直接使用这些工具：

```cpp
async_simple::coro::Lazy<std::string> my_api(std::string input) {
  // 非阻塞 sleep，底层使用 asio::steady_timer
  co_await async_simple::coro::sleep(std::chrono::milliseconds(100));

  // 阻塞操作交给线程池
  auto result = co_await dcb::spawn_blocking([&] {
    return heavyComputation(input);
  });

  co_return result;
}
```

## spawn / spawn_detached

如果你不在协程上下文里（比如普通函数、回调），想把一个 Lazy 投到 io 线程执行：

```cpp
#include "async_simple/coro/SyncAwait.h"

// 启动并等待结果（不能在 io 线程调用，会死锁）
auto result = async_simple::coro::syncAwait(
    dcb::spawn(my_coroutine()));

// 启动后丢弃结果（fire-and-forget）
dcb::spawn_detached(my_coroutine());
```

`dcb::spawn(lazy)` 返回一个已经绑定到 Runtime executor 的 `RescheduleLazy<T>`。支持：

- `syncAwait(...)` — 阻塞当前线程直到完成
- `.start(callback)` — 自定义完成回调
- `spawn_detached(...)` — 直接启动，忽略结果与异常

:::caution
不要在 io 线程上调用 `syncAwait(...)`，否则被等待的协程也需要 io 线程恢复，会自死锁。
:::

## spawn_blocking

把阻塞任务放到 `thread_pool` 上运行，io 线程不阻塞：

```cpp
async_simple::coro::Lazy<int> compute(int n) {
  auto result = co_await dcb::spawn_blocking([n] {
    // 在 pool 线程执行，可以 sleep / 同步 IO
    int sum = 0;
    for (int i = 1; i <= n; ++i) sum += i;
    return sum;
  });

  // 异常会从 pool 线程捕获，并在 co_await 处重新抛出
  co_return result;
}
```

## 异步 sleep

`async_simple::coro::sleep(dur)` 在绑定了 `AsioExecutor` 的协程中是非阻塞的：AsioExecutor 重写了 async-simple 的 `schedule(Func, Duration)`，用 `asio::steady_timer` 实现，不会占用线程。

```cpp
#include "async_simple/coro/Sleep.h"

async_simple::coro::Lazy<std::string> delayed_echo(std::string msg) {
  co_await async_simple::coro::sleep(std::chrono::seconds(1));
  co_return msg;
}
```

:::caution
如果 Lazy 没有 `.via(ex)` 绑定到 `AsioExecutor`，sleep 可能退化为 async-simple 默认的“另开线程 sleep”实现。业务代码通常都通过 wire dispatch 或 `dcb::spawn` 运行在 AsioExecutor 上。
:::

## 跨协程通信：channel

`dart_cpp_bridge/channel.hpp` 提供两类 Tokio 风格 channel，用于协程间或跨线程传递数据。`co::mpsc::unbounded<T>` 底层使用 **moodycamel::ConcurrentQueue** 作为无锁队列。

### oneshot — 一次性请求/响应

```cpp
auto [tx, rx] = co::oneshot::channel<std::string>();

// 任意线程发送
tx.send("hello");

// 在协程中接收
auto value = co_await rx.recv();  // std::optional<std::string>
if (value) { /* ... */ }
```

### mpsc — 多生产者单消费者

```cpp
auto [tx, rx] = co::mpsc::unbounded<int>();

// 任意线程/多个生产者发送
tx.send(1);
tx.send(2);

// 在协程中接收
while (auto v = co_await rx.recv()) {
  // 处理 v
}
```

`Sender` 是线程安全的，`send()` 永不阻塞。`Receiver` 的 `recv()` 不能并发调用。

:::caution[单消费者]
`co::oneshot` 和 `co::mpsc` 都是**单消费者**模型：

- `oneshot` 只能接收一次
- `mpsc` 可以有多个 `Sender` 同时发，但只能有一个 `Receiver` 且该 `Receiver` 不能多个线程/协程并发调用 `recv()`

如果需要多消费者，需要在单个 `recv()` 循环里把任务分发给多个处理协程。
:::

:::caution[channel_value 约束]
channel 要求值类型 `T` 满足：

```cpp
std::movable<T> && !std::is_const_v<T> && !std::is_volatile_v<T>
```

即 `T` 必须是**可移动**的，且不能是 `const` / `volatile` 类型。如果类型不可移动（比如包含 `std::mutex` 或 `const` 成员），可以改用 `std::shared_ptr<T>` 或 `std::unique_ptr<T>` 包装后再入队。
:::

## 创建独立的 asio 运行时

默认 `dcb::Runtime` 是进程级单例。如果你需要与主 Runtime 隔离的独立事件循环（比如一个专用 Worker 线程），可以自己组装：

```cpp
#include "dart_cpp_bridge/asio_executor.hpp"
#include <asio/io_context.hpp>
#include <asio/executor_work_guard.hpp>

asio::io_context ioc;
auto guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
    ioc.get_executor());
auto ex = std::make_unique<dcb::AsioExecutor>(ioc);

std::thread t([&] {
  ex->set_io_thread_id(std::this_thread::get_id());
  ioc.run();
});

// 之后就可以在这个独立 executor 上运行协程：
// my_coroutine().via(ex.get()).start([](auto&&) {});
```

完整实现参考 `examples/multi_runtime_demo/worker_runtime.hpp`。

## 线程规则

:::caution
- 永远不要阻塞 `io_context` 线程
- 阻塞操作用 `dcb::spawn_blocking`
- `syncAwait` 不能在 io 线程调用（`AsioExecutor::currentThreadInExecutor()` 会断言失败）
- 跨线程/运行时通信优先用 `channel`，而不是裸锁 + 条件变量
:::

## 完整示例

- `examples/base_demo` — 基础 sync / async / stream / DartFn
- `examples/multi_runtime_demo` — 独立 AsioExecutor 运行时 + channel
- `examples/foreign_runtime_demo` — 非 asio 运行时通过 `ForeignExecutor` 接入

## 延伸阅读

- [async-simple 协程入门](/dart_cpp_bridge/guides/fundamentals/async-simple/)
- [架构设计](/dart_cpp_bridge/guides/fundamentals/architecture/)
- [多运行时](/dart_cpp_bridge/guides/advanced/multi-runtime/)
- [外部运行时集成](/dart_cpp_bridge/guides/advanced/foreign-runtime/)
- [纯 C 桥接 API](/dart_cpp_bridge/guides/advanced/cbridge/)
