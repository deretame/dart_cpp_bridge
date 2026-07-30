---
title: 多个运行时
description: 在 dart_cpp_bridge 主 Runtime 之外创建独立的 asio + async-simple 运行时，并通过协程 channel 与主 Runtime 通信
---

`dart_cpp_bridge` 的 `dcb::Runtime` 是进程级单例，不能被复制。如果你需要另一个独立的事件循环（比如把重计算、独立子系统、第三方异步库隔离在自己的线程里），可以自己组装一个 **WorkerRuntime**：一个 `asio::io_context` + `dcb::AsioExecutor` + 一个 `std::thread`。

## 最小 WorkerRuntime

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

    // 保持 io_context 在没有任务时也不退出
    guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        ioc_.get_executor());
    // 创建独立的 async-simple Executor
    executor_ = std::make_unique<dcb::AsioExecutor>(ioc_);
    // 启动事件循环线程
    thread_ = std::make_unique<std::thread>([this] { ioc_.run(); });
  }

  void stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (guard_) guard_.reset();  // 允许 io_context run out of work
    if (thread_ && thread_->joinable()) thread_->join();
    thread_.reset();
    executor_.reset();
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  dcb::AsioExecutor* executor() { return executor_.get(); }
  asio::io_context& io() { return ioc_; }

  // 把一个 Lazy 工厂投递到本 Worker 的事件循环上启动
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

关键点：

- `asio::executor_work_guard` 防止 `io_context` 因为没有任务而立即退出
- `dcb::AsioExecutor` 让 `async_simple::coro::Lazy<T>` 可以调度到 `io_context`
- `spawn()` 用 `asio::post` 把协程丢到 Worker 线程，并通过 `shared_ptr` 保持 lambda 工厂存活到协程结束

## 与主 Runtime 通信

不同运行时之间**不共享线程、不共享内存**，通信只能通过 `co::oneshot` / `co::mpsc` channel。

### oneshot：单次请求/回复

```cpp
#include "dart_cpp_bridge/channel.hpp"

async_simple::coro::Lazy<std::string> process_on_worker(
    WorkerRuntime* worker, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  worker->spawn([tx = std::move(tx), input = std::move(input)]() mutable -> async_simple::coro::Lazy<> {
    // 在 Worker 线程上执行
    std::string result = "processed: " + input;
    tx.send(std::move(result));
    co_return;
  });

  // 主 Runtime 协程挂起等待，不阻塞 io 线程
  auto reply = co_await rx.recv();
  if (!reply) throw std::runtime_error("worker dropped");
  co_return *reply;
}
```

### mpsc：Worker 持续产生数据

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
    // tx 析构 → channel 关闭
    co_return;
  });

  while (auto item = co_await rx.recv()) {
    on_item(*item);
  }
  co_return;
}
```

### Pipeline：Worker A → Worker B

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

### Fan-out：并行发到多个 Worker

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

## 在 Worker 里调用 Dart 函数

Worker 的 `AsioExecutor` 已经完整实现了 `async_simple::Executor`，所以协程里可以直接 `co_await` DartFn，Dart 回复时会自动调度回 Worker 线程。

```cpp
#include "dart_cpp_bridge/dart_fn.hpp"

async_simple::coro::Lazy<std::string> call_dart_from_worker(
    WorkerRuntime* worker,
    dcb::DartFn<std::string(std::string)> callback,
    std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  worker->spawn([tx = std::move(tx), cb = std::move(callback),
                 input = std::move(input)]() mutable -> async_simple::coro::Lazy<> {
    // 在 Worker 线程上非阻塞等待 Dart 回调
    auto result = co_await cb(input);
    tx.send(std::move(result));
    co_return;
  });

  auto reply = co_await rx.recv();
  co_return reply.value_or("worker dropped");
}
```

原理：

- `DartFn::operator()` 内部创建 oneshot channel 并 `co_await rx.recv()`
- channel 的 `coAwait(ex)` 捕获当前协程的 executor（即 Worker 的 `AsioExecutor`）
- Dart 回复时，`wake_waiter(h, ex)` 把协程恢复投递回 Worker 线程

## 注意事项

- **Worker 的 `executor()` 必须比上面运行的协程活得更长**：不要在 `stop()` 之后还在等待 Worker 上的协程完成
- **不要阻塞 Worker 线程**：和主 Runtime 一样，`io_context` 线程不能阻塞；阻塞任务用 `dcb::spawn_blocking`
- **不要在 Worker 线程上 `syncAwait`**：会死锁
- **Worker 之间不要直接共享状态**：所有跨 Worker 通信都走 channel；如果必须共享数据，用 `std::mutex` 自己保护，但这不是推荐模式
- **多 Worker 可以共享同一个 Dart 桥接**：DartFn 通过主 Runtime 的 Session 路由，任何 Worker 都能调用

## 完整示例

参考 `examples/multi_runtime_demo/`：

- `worker_runtime.hpp` — 上面的 `WorkerRuntime` 类
- `native/api/multi_runtime_api.h` — 桥接 API 声明
- `native/api_impl/multi_runtime_api.cpp` — oneshot / mpsc / pipeline / fan-out / DartFn 的实现

## 延伸阅读

- [基础运行时](/dart_cpp_bridge/guides/fundamentals/runtime/) — `spawn`、`spawn_blocking`、channel、sleep
- [async-simple 协程入门](/dart_cpp_bridge/guides/fundamentals/async-simple/) — `Lazy`、`Executor`、co_await 行为
- [外部运行时集成](/dart_cpp_bridge/guides/advanced/foreign-runtime/) — 把非 asio 事件循环（libuv、glib）接入 bridge
