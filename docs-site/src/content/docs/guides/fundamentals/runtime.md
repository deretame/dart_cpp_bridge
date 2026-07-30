---
title: Built-in Runtime
description: The built-in asio + async-simple runtime in dart_cpp_bridge, plus foundational tools such as spawn, spawn_blocking, channel, and sleep.
---

`dart_cpp_bridge` includes a built-in **asio + async-simple** runtime on the C++ side. By default, it starts automatically when `DartCppBridge.init()` is called from Dart; business code can write `async_simple::coro::Lazy<T>` coroutines directly without creating its own `io_context` or `Executor`. This chapter introduces the foundational tools and how to use them directly.

## Libraries Ready to Use

The bridge already pulls in and initializes these for you via FetchContent:

- **Asio standalone** — `asio::io_context` single-threaded event loop + `asio::thread_pool` blocking thread pool
- **async-simple** — `async_simple::coro::Lazy<T>` coroutines and `async_simple::Executor` scheduling model
- **moodycamel::ConcurrentQueue** — the lock-free queue underlying `co::mpsc::unbounded<T>` (`concurrentqueue.h`)

Common headers:

```cpp
#include "dart_cpp_bridge/runtime.hpp"        // Runtime, spawn, spawn_blocking
#include "dart_cpp_bridge/asio_executor.hpp"  // AsioExecutor
#include "dart_cpp_bridge/channel.hpp"        // co::oneshot / co::mpsc
#include "async_simple/coro/Lazy.h"             // Lazy<T>
#include "async_simple/coro/Sleep.h"            // sleep
```

## Runtime Singleton

`dcb::Runtime` is a process-wide singleton that holds:

- `asio::io_context` — single-threaded event loop (io thread)
- `dcb::AsioExecutor` — async-simple's `Executor` implementation that schedules coroutines back onto the `io_context`
- `asio::thread_pool` — blocking worker pool (default 4 threads, adjustable via `set_pool_threads()`)

```cpp
#include "dart_cpp_bridge/runtime.hpp"

// Manual start/stop (normally done automatically by Dart-side init; C++ unit tests need to call it manually)
dcb::Runtime::instance().start();
dcb::Runtime::instance().stop();
```

Main interfaces:

| Interface | Purpose |
|---|---|
| `start()` / `stop()` | Start/stop the io thread and thread pool |
| `running()` | Whether it has started |
| `io()` | Get `asio::io_context&` |
| `pool()` | Get `asio::thread_pool&` |
| `executor()` | Get `dcb::AsioExecutor*` |
| `spawn_on_asio(factory)` | Post a Lazy factory from a non-coroutine context to the io thread to start |

## `co_await` Directly in Business Coroutines

Business functions invoked by wire dispatch already run as `Lazy<T>` on the io thread, so you can use these tools directly:

```cpp
async_simple::coro::Lazy<std::string> my_api(std::string input) {
  // Non-blocking sleep, backed by asio::steady_timer
  co_await async_simple::coro::sleep(std::chrono::milliseconds(100));

  // Hand blocking work off to the thread pool
  auto result = co_await dcb::spawn_blocking([&] {
    return heavyComputation(input);
  });

  co_return result;
}
```

## spawn / spawn_detached

When you are not in a coroutine context (e.g., a normal function or callback) and want to post a Lazy to the io thread for execution:

```cpp
#include "async_simple/coro/SyncAwait.h"

// Start and wait for the result (do not call on the io thread, or it will deadlock)
auto result = async_simple::coro::syncAwait(
    dcb::spawn(my_coroutine()));

// Start and discard the result (fire-and-forget)
dcb::spawn_detached(my_coroutine());
```

`dcb::spawn(lazy)` returns a `RescheduleLazy<T>` already bound to the Runtime executor. It supports:

- `syncAwait(...)` — block the current thread until completion
- `.start(callback)` — custom completion callback
- `spawn_detached(...)` — start directly, ignoring results and exceptions

:::caution
Do not call `syncAwait(...)` on the io thread. The coroutine being awaited also needs the io thread to resume, causing a self-deadlock.
:::

## spawn_blocking

Run blocking tasks on the `thread_pool` without blocking the io thread:

```cpp
async_simple::coro::Lazy<int> compute(int n) {
  auto result = co_await dcb::spawn_blocking([n] {
    // Runs on a pool thread; can sleep or do synchronous IO
    int sum = 0;
    for (int i = 1; i <= n; ++i) sum += i;
    return sum;
  });

  // Exceptions are caught on the pool thread and rethrown at the co_await site
  co_return result;
}
```

## Asynchronous sleep

`async_simple::coro::sleep(dur)` is non-blocking in a coroutine bound to `AsioExecutor`: AsioExecutor overrides async-simple's `schedule(Func, Duration)` and uses `asio::steady_timer`, so it does not occupy a thread.

```cpp
#include "async_simple/coro/Sleep.h"

async_simple::coro::Lazy<std::string> delayed_echo(std::string msg) {
  co_await async_simple::coro::sleep(std::chrono::seconds(1));
  co_return msg;
}
```

:::caution
If the Lazy is not bound to `AsioExecutor` via `.via(ex)`, sleep may fall back to async-simple's default "spawn another thread to sleep" implementation. Business code usually runs on AsioExecutor through wire dispatch or `dcb::spawn`.
:::

## Cross-Coroutine Communication: channel

`dart_cpp_bridge/channel.hpp` provides two Tokio-style channel types for passing data between coroutines or across threads. `co::mpsc::unbounded<T>` uses **moodycamel::ConcurrentQueue** as its lock-free queue underneath.

### oneshot — one-shot request/response

```cpp
auto [tx, rx] = co::oneshot::channel<std::string>();

// Send from any thread
tx.send("hello");

// Receive in a coroutine
auto value = co_await rx.recv();  // std::optional<std::string>
if (value) { /* ... */ }
```

### mpsc — multi-producer, single-consumer

```cpp
auto [tx, rx] = co::mpsc::unbounded<int>();

// Send from any thread / multiple producers
tx.send(1);
tx.send(2);

// Receive in a coroutine
while (auto v = co_await rx.recv()) {
  // process v
}
```

`Sender` is thread-safe and `send()` never blocks. `Receiver::recv()` must not be called concurrently.

:::caution[Single-consumer]
`co::oneshot` and `co::mpsc` are both **single-consumer** models:

- `oneshot` can only receive once
- `mpsc` may have multiple `Sender`s sending at the same time, but there can be only one `Receiver`, and that `Receiver` must not call `recv()` concurrently from multiple threads or coroutines

If you need multiple consumers, distribute tasks from a single `recv()` loop to multiple processing coroutines.
:::

:::caution[channel value constraints]
The channel requires the value type `T` to satisfy:

```cpp
std::movable<T> && !std::is_const_v<T> && !std::is_volatile_v<T>
```

That is, `T` must be **movable** and must not be a `const` or `volatile` type. If the type is immovable (e.g., it contains a `std::mutex` or `const` member), wrap it in `std::shared_ptr<T>` or `std::unique_ptr<T>` before enqueuing.
:::

## Creating a Standalone Asio Runtime

By default `dcb::Runtime` is a process-wide singleton. If you need an independent event loop isolated from the main Runtime (for example, a dedicated worker thread), you can assemble one yourself:

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

// You can then run coroutines on this standalone executor:
// my_coroutine().via(ex.get()).start([](auto&&) {});
```

For the full implementation, see `examples/multi_runtime_demo/worker_runtime.hpp`.

## Threading Rules

:::caution
- Never block the `io_context` thread
- Use `dcb::spawn_blocking` for blocking operations
- `syncAwait` must not be called on the io thread (`AsioExecutor::currentThreadInExecutor()` will assert)
- Prefer `channel` for cross-thread / cross-runtime communication instead of raw locks + condition variables
:::

## Full Examples

- `examples/base_demo` — basic sync / async / stream / DartFn
- `examples/multi_runtime_demo` — standalone AsioExecutor runtime + channel
- `examples/foreign_runtime_demo` — non-asio runtime integration via `ForeignExecutor`

## Further Reading

- [async-simple Coroutine Basics](/dart_cpp_bridge/guides/fundamentals/async-simple/)
- [Architecture](/dart_cpp_bridge/guides/fundamentals/architecture/)
- [Multi-Runtime](/dart_cpp_bridge/guides/advanced/multi-runtime/)
- [Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/)
- [Pure C Bridge API](/dart_cpp_bridge/guides/advanced/cbridge/)
