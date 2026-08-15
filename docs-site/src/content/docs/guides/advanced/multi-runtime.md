---
title: "v1 archive: Multiple Runtimes"
description: Create independent asio + async-simple runtimes outside the dart_cpp_bridge main Runtime, and communicate with the main Runtime through coroutine channels.
sidebar:
  hidden: true
---

:::caution[v1 archive]
This page documents the v1 AsioExecutor / async-simple implementation. For v2,
adapt an independent event loop as a plain stdexec scheduler; see the current
[foreign runtime guide](/dart_cpp_bridge/guides/advanced/foreign-runtime/).
:::

`dart_cpp_bridge`'s `dcb::Runtime` is a process-level singleton and cannot be copied. If you need another independent event loop (for example, to isolate heavy computation, independent subsystems, or third-party async libraries in their own thread), you can assemble a **WorkerRuntime** yourself: an `asio::io_context` + `dcb::AsioExecutor` + a `std::thread`.

## Minimal WorkerRuntime

```cpp
#pragma once

#include "dart_cpp_bridge/asio_executor.hpp"
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/executor_work_guard.hpp>
#include <async_simple/coro/Lazy.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

class WorkerRuntime {
 public:
  explicit WorkerRuntime(std::string name) : name_(std::move(name)) {}
  ~WorkerRuntime() { stop(); }

  WorkerRuntime(const WorkerRuntime&) = delete;
  WorkerRuntime& operator=(const WorkerRuntime&) = delete;

  void start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);

    // Keep io_context alive even when there is no work
    guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        ioc_.get_executor());
    // Create an independent async-simple Executor
    executor_ = std::make_unique<dcb::AsioExecutor>(ioc_);
    // Start the event loop thread
    thread_ = std::make_unique<std::thread>([this] { ioc_.run(); });
  }

  void stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (guard_) guard_.reset();  // Allow io_context to run out of work
    if (thread_ && thread_->joinable()) thread_->join();
    thread_.reset();
    executor_.reset();
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  dcb::AsioExecutor* executor() { return executor_.get(); }
  asio::io_context& io() { return ioc_; }

  // Post a Lazy factory onto this Worker's event loop to start
  template <class LazyFactory>
  void spawn(LazyFactory&& factory) {
    auto* ex = executor_.get();
    asio::post(ioc_, [factory = std::forward<LazyFactory>(factory), ex]() mutable {
      auto holder = std::make_shared<std::decay_t<decltype(factory)>>(std::move(factory));
      auto lazy = (*holder)();
      std::move(lazy).via(ex).start([holder](auto&&) { (void)holder; });
    });
  }

 private:
  std::string name_;
  asio::io_context ioc_;
  std::unique_ptr<dcb::AsioExecutor> executor_;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
  std::unique_ptr<std::thread> thread_;
  std::atomic<bool> running_{false};
};
```

Key points:

- `asio::executor_work_guard` keeps the `io_context` from exiting immediately when there is no work.
- `dcb::AsioExecutor` lets `async_simple::coro::Lazy<T>` be scheduled on the `io_context`.
- `spawn()` uses `asio::post` to dispatch the coroutine to the Worker thread and uses a `shared_ptr` to keep the lambda factory alive until the coroutine ends.

## Communicating with the Main Runtime

Runtimes **do not share threads or memory**; communication can only go through `co::oneshot` / `co::mpsc` channels.

### oneshot: single request/reply

```cpp
#include "dart_cpp_bridge/channel.hpp"

async_simple::coro::Lazy<std::string> process_on_worker(
    WorkerRuntime* worker, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  worker->spawn([tx = std::move(tx), input = std::move(input)]() mutable -> async_simple::coro::Lazy<> {
    // Run on the Worker thread
    std::string result = "processed: " + input;
    tx.send(std::move(result));
    co_return;
  });

  // The main Runtime coroutine suspends to wait, without blocking the io thread
  auto reply = co_await rx.recv();
  if (!reply) throw std::runtime_error("worker dropped");
  co_return *reply;
}
```

### mpsc: Worker continuously produces data

```cpp
async_simple::coro::Lazy<> consume_worker_stream(
    WorkerRuntime* worker,
    std::function<void(std::string)> on_item) {
  auto [tx, rx] = co::mpsc::unbounded<std::string>();

  worker->spawn([tx = std::move(tx)]() mutable -> async_simple::coro::Lazy<> {
    for (int i = 0; i < 5; ++i) {
      tx.send("item_" + std::to_string(i));
      co_await async_simple::coro::sleep(std::chrono::milliseconds(100));
    }
    // tx is destroyed → channel closes
    co_return;
  });

  while (auto item = co_await rx.recv()) {
    on_item(*item);
  }
  co_return;
}
```

### Pipeline: Worker A → Worker B

```cpp
async_simple::coro::Lazy<std::string> pipeline(
    WorkerRuntime* a, WorkerRuntime* b, std::string message) {
  auto [tx_ab, rx_ab] = co::oneshot::channel<std::string>();
  auto [tx_final, rx_final] = co::oneshot::channel<std::string>();

  a->spawn([tx = std::move(tx_ab), message]() mutable -> async_simple::coro::Lazy<> {
    tx.send("A{" + message + "}");
    co_return;
  });

  b->spawn([rx = std::move(rx_ab), tx = std::move(tx_final)]() mutable -> async_simple::coro::Lazy<> {
    auto from_a = co_await rx.recv();
    tx.send("B[" + from_a.value_or("lost") + "]");
    co_return;
  });

  auto result = co_await rx_final.recv();
  co_return result.value_or("lost");
}
```

### Fan-out: send in parallel to multiple Workers

```cpp
async_simple::coro::Lazy<std::pair<std::string, std::string>> fan_out(
    WorkerRuntime* a, WorkerRuntime* b, std::string message) {
  auto [tx_a, rx_a] = co::oneshot::channel<std::string>();
  auto [tx_b, rx_b] = co::oneshot::channel<std::string>();

  a->spawn([tx = std::move(tx_a), message]() mutable -> async_simple::coro::Lazy<> {
    tx.send("A:" + message);
    co_return;
  });
  b->spawn([tx = std::move(tx_b), message]() mutable -> async_simple::coro::Lazy<> {
    tx.send("B:" + message);
    co_return;
  });

  auto reply_a = co_await rx_a.recv();
  auto reply_b = co_await rx_b.recv();
  co_return std::make_pair(reply_a.value_or("lost"), reply_b.value_or("lost"));
}
```

## Calling Dart Functions from a Worker

The Worker's `AsioExecutor` fully implements `async_simple::Executor`, so inside a coroutine you can directly `co_await` a `DartFn`; when Dart replies, it is automatically scheduled back to the Worker thread.

```cpp
#include "dart_cpp_bridge/dart_fn.hpp"

async_simple::coro::Lazy<std::string> call_dart_from_worker(
    WorkerRuntime* worker,
    dcb::DartFn<std::string(std::string)> callback,
    std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  worker->spawn([tx = std::move(tx), cb = std::move(callback),
                 input = std::move(input)]() mutable -> async_simple::coro::Lazy<> {
    // Await the Dart callback on the Worker thread without blocking
    auto result = co_await cb(input);
    tx.send(std::move(result));
    co_return;
  });

  auto reply = co_await rx.recv();
  co_return reply.value_or("worker dropped");
}
```

How it works:

- `DartFn::operator()` internally creates an oneshot channel and `co_await`s `rx.recv()`.
- The channel's `coAwait(ex)` captures the current coroutine's executor (the Worker's `AsioExecutor`).
- When Dart replies, `wake_waiter(h, ex)` resumes the coroutine and posts it back to the Worker thread.

## Notes

- **The Worker's `executor()` must outlive the coroutines running on it**: do not wait for coroutines on the Worker to complete after `stop()`.
- **Do not block the Worker thread**: like the main Runtime, the `io_context` thread must not block; use `dcb::spawn_blocking` for blocking tasks.
- **Do not call `syncAwait` on the Worker thread**: it will deadlock.
- **Do not share state directly between Workers**: all cross-Worker communication goes through channels; if you must share data, protect it with `std::mutex` yourself, but this is not the recommended pattern.
- **Multiple Workers can share the same Dart bridge**: `DartFn` is routed through the main Runtime's `Session`, so any Worker can call it.

## Full Example

See `examples/multi_runtime_demo/`:

- `worker_runtime.hpp` — the `WorkerRuntime` class shown above
- `native/api/multi_runtime_api.h` — bridge API declarations
- `native/api_impl/multi_runtime_api.cpp` — oneshot / mpsc / pipeline / fan-out / DartFn implementations

## Further Reading

- [Fundamentals: Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/) — `spawn`, `spawn_blocking`, channel, sleep
- [async-simple Coroutine Basics](/dart_cpp_bridge/guides/fundamentals/async-simple/) — `Lazy`, `Executor`, `co_await` behavior
- [Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/) — integrating non-asio event loops (libuv, glib) with the bridge
