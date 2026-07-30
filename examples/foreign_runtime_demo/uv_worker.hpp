#pragma once

// UvWorker — Independent runtime based on libuv, integrated into the bridge channel system via ForeignExecutor.
//
// Demonstrates how a non-asio event loop participates in co::oneshot / co::mpsc communication:
//   bridge schedule → uv_async_send → loop thread drain → execute coroutines/tasks

#include <uv.h>

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/foreign_runtime.h"

namespace foreign_demo {

class UvWorker {
 public:
  explicit UvWorker(std::string name) : name_(std::move(name)) {}

  ~UvWorker() { stop(); }

  UvWorker(const UvWorker&) = delete;
  UvWorker& operator=(const UvWorker&) = delete;

  void start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);

    uv_loop_init(&loop_);

    // async handle used to wake the loop across threads
    uv_async_init(&loop_, &async_, [](uv_async_t* handle) {
      auto* self = static_cast<UvWorker*>(handle->data);
      self->drain_pending();
    });
    async_.data = this;

    // Register with bridge to obtain a ForeignExecutor
    foreign_id_ = dcb_foreign_register(name_.c_str(), &schedule_callback, this);

    // Start the loop thread
    thread_ = std::thread([this] {
      // Mark ourselves on the loop thread so currentThreadInExecutor() works correctly
      dcb_foreign_mark_loop_thread(foreign_id_);
      uv_run(&loop_, UV_RUN_DEFAULT);
    });
  }

  void stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);

    // Unregister (bridge will no longer schedule to us afterwards)
    if (foreign_id_) {
      dcb_foreign_unregister(foreign_id_);
      foreign_id_ = 0;
    }

    // Stop the loop
    uv_stop(&loop_);
    uv_async_send(&async_);  // Wake it up so it exits uv_run

    if (thread_.joinable()) thread_.join();
    uv_loop_close(&loop_);
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  const std::string& name() const { return name_; }
  uint32_t foreign_id() const { return foreign_id_; }

  /// Get ForeignExecutor (used for channel coAwait / Lazy.via())
  dcb::ForeignExecutor* executor() {
    return static_cast<dcb::ForeignExecutor*>(dcb_foreign_executor(foreign_id_));
  }

  /// Execute a task on the libuv loop thread (C++ convenience interface)
  void post(void (*fn)(void*), void* userdata) {
    schedule_callback(fn, userdata, this);
  }

 private:
  /// Bridge's ForeignExecutor calls this function to schedule tasks (static, C-linkage compatible)
  static void schedule_callback(void (*fn)(void*), void* userdata, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    {
      std::lock_guard lock(self->mu_);
      self->pending_.push({fn, userdata});
    }
    uv_async_send(&self->async_);  // Thread-safe loop wake-up
  }

  /// Execute all pending tasks on the loop thread
  void drain_pending() {
    std::queue<std::pair<void (*)(void*), void*>> batch;
    {
      std::lock_guard lock(mu_);
      batch.swap(pending_);
    }
    while (!batch.empty()) {
      auto [fn, ud] = batch.front();
      batch.pop();
      fn(ud);  // Execute (e.g. coroutine-resume trampoline)
    }
  }

  std::string name_;
  uv_loop_t loop_{};
  uv_async_t async_{};
  std::mutex mu_;
  std::queue<std::pair<void (*)(void*), void*>> pending_;
  std::thread thread_;
  uint32_t foreign_id_{0};
  std::atomic<bool> running_{false};
};

}  // namespace foreign_demo
