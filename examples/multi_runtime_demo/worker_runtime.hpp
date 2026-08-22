#pragma once

// WorkerRuntime — an independent asio event loop with a stdexec scheduler.
//
// Each worker owns its own io_context, scheduler and thread. Communication
// with other runtimes is exclusively through co::mpsc / co::oneshot channels;
// channel waits suspend a stdexec::task and never block the worker thread.

#include "dart_cpp_bridge/runtime.hpp"

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>

#include <atomic>
#include <cstdio>
#include <exception>
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
    guard_ = std::make_unique<DCB_ASIO_NS::executor_work_guard<DCB_ASIO_NS::io_context::executor_type>>(
        ioc_.get_executor());
    scheduler_ = std::make_shared<dcb::IoContextScheduler>(ioc_);
    thread_ = std::make_unique<std::thread>([this] { ioc_.run(); });
  }

  void stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    guard_.reset();
    if (thread_ && thread_->joinable()) thread_->join();
    thread_.reset();
    scheduler_.reset();
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  const std::string& name() const { return name_; }
  dcb::IoContextScheduler& scheduler() { return *scheduler_; }
  DCB_ASIO_NS::io_context& io() { return ioc_; }

  /// Launch a stdexec::task/sender on this worker's event loop.
  template <class S>
  void spawn(S&& sndr) {
    const std::string name = name_;
    exec::start_detached(
        stdexec::starts_on(*scheduler_, std::forward<S>(sndr))
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
  DCB_ASIO_NS::io_context ioc_;
  std::shared_ptr<dcb::IoContextScheduler> scheduler_;
  std::unique_ptr<DCB_ASIO_NS::executor_work_guard<DCB_ASIO_NS::io_context::executor_type>> guard_;
  std::unique_ptr<std::thread> thread_;
  std::atomic<bool> running_{false};
};

}  // namespace multi_rt
