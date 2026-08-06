#pragma once

// WorkerRuntime — an independent asio + async-simple event loop.
//
// Each WorkerRuntime owns:
//   - its own asio::io_context (single-threaded)
//   - its own AsioExecutor (for Lazy coroutine scheduling)
//   - its own std::thread
//
// Communication with other runtimes is exclusively through co::mpsc / co::oneshot
// channels. send() is non-blocking; recv() suspends the coroutine (not the thread).

#include "dart_cpp_bridge/runtime.hpp"

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/executor_work_guard.hpp>

#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace multi_rt {

class WorkerRuntime {
 public:
  explicit WorkerRuntime(std::string name) : name_(std::move(name)) {}

  ~WorkerRuntime() { stop(); }

  WorkerRuntime(const WorkerRuntime&) = delete;
  WorkerRuntime& operator=(const WorkerRuntime&) = delete;

  void start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
        ioc_.get_executor());
    executor_ = std::make_unique<dcb::AsioExecutor>(ioc_);
    thread_ = std::make_unique<std::thread>([this] { ioc_.run(); });
  }

  void stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    if (guard_) {
      guard_.reset();  // allow io_context to run out of work
    }
    if (thread_ && thread_->joinable()) {
      thread_->join();
    }
    thread_.reset();
    executor_.reset();
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  const std::string& name() const { return name_; }
  dcb::AsioExecutor* executor() { return executor_.get(); }
  asio::io_context& io() { return ioc_; }

  /// Spawn a Lazy coroutine on this worker's event loop.
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

}  // namespace multi_rt
