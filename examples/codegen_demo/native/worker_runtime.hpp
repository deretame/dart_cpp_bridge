#pragma once

// WorkerRuntime — an independent asio event loop with a stdexec scheduler.
//
// Each WorkerRuntime owns:
//   - its own asio::io_context (single-threaded)
//   - its own dcb::IoContextScheduler (stdexec scheduler over the loop)
//   - its own std::thread
//
// Communication with other runtimes is exclusively through co::mpsc /
// co::oneshot channels. send() is non-blocking; recv() suspends the coroutine
// (not the thread).

#include "dart_cpp_bridge/runtime.hpp"

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>
#include <exec/task.hpp>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/executor_work_guard.hpp>

#include <atomic>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace demo {

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
    sched_ = std::make_shared<dcb::IoContextScheduler>(ioc_);
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
    sched_.reset();
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  const std::string& name() const { return name_; }
  dcb::IoContextScheduler& scheduler() { return *sched_; }
  asio::io_context& io() { return ioc_; }

  /// Launch a sender/coroutine (exec::task) on this worker's event loop.
  template <class S>
  void spawn(S&& sndr) {
    const std::string name = name_;
    exec::start_detached(
        stdexec::starts_on(*sched_, std::forward<S>(sndr))
        | stdexec::upon_error([name](std::exception_ptr ep) noexcept {
            try {
              std::rethrow_exception(ep);
            } catch (const std::exception& e) {
              std::fprintf(stderr, "[WorkerRuntime:%s] task error: %s\n",
                           name.c_str(), e.what());
            } catch (...) {
              std::fprintf(stderr, "[WorkerRuntime:%s] task error: unknown\n",
                           name.c_str());
            }
          })
        | stdexec::upon_stopped([name]() noexcept {
            std::fprintf(stderr, "[WorkerRuntime:%s] task stopped\n",
                         name.c_str());
          }));
  }

 private:
  std::string name_;
  asio::io_context ioc_;
  std::shared_ptr<dcb::IoContextScheduler> sched_;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
  std::unique_ptr<std::thread> thread_;
  std::atomic<bool> running_{false};
};

}  // namespace demo
