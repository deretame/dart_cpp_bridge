#pragma once

// UvWorker — Independent runtime based on libuv, integrated into the bridge channel system via ForeignExecutor.
//
// Demonstrates how a non-asio event loop participates in co::oneshot / co::mpsc communication:
//   bridge schedule → uv_async_send → loop thread drain → execute coroutines/tasks

#include <uv.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/foreign_runtime.h"

namespace demo {

class UvWorker {
 public:
  explicit UvWorker(std::string name) : name_(std::move(name)) {}

  ~UvWorker() { stop(); }

  UvWorker(const UvWorker&) = delete;
  UvWorker& operator=(const UvWorker&) = delete;

  void start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    stop_all_requested_.store(false, std::memory_order_release);
    stop_all_done_.store(false, std::memory_order_release);
    pending_closes_.store(0, std::memory_order_release);

    uv_loop_init(&loop_);

    // async handle used to wake the loop across threads
    uv_async_init(&loop_, &async_, [](uv_async_t* handle) {
      auto* self = static_cast<UvWorker*>(handle->data);
      self->drain_pending();
    });
    async_.data = this;

    // Register with bridge to obtain a ForeignExecutor. Native timer
    // callbacks let co_await sleep() use uv_timer_t instead of a waiter
    // thread per sleep.
    foreign_id_ = dcb_foreign_register_ex(
        name_.c_str(), &schedule_callback, &schedule_after_callback,
        &cancel_after_callback, this);

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
    // Clean up any pending native timers on the loop thread before exit
    // (uv_loop_close fails if live handles remain). We wait for the loop
    // thread to drain them first, because uv_stop can make uv_run exit
    // before the async wake-up is processed.
    {
      std::lock_guard lock(mu_);
      if (!live_timers_.empty()) {
        pending_.push({&stop_all_timers_trampoline, this});
        stop_all_requested_.store(true, std::memory_order_release);
      }
    }
    if (stop_all_requested_.load(std::memory_order_acquire)) {
      uv_async_send(&async_);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!stop_all_done_.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
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

  // ─── Native timer callbacks (dcb_foreign_register_ex) ─────────────────────
  //
  // schedule_after is invoked on the loop thread (the awaiting coroutine
  // runs there), so uv_timer_init is safe. cancel_after may be called from
  // any thread; it only enqueues a stop task and never touches the timer
  // outside the loop thread. Liveness is tracked in live_timers_ (guarded by
  // mu_), so cancel is a safe no-op for timers that already fired.

  struct TimerCallbackBox {
    void (*fn)(void*);
    void* userdata;
    UvWorker* self;
  };

  struct CancelTimerTask {
    UvWorker* self;
    uv_timer_t* timer;
  };

  static void* schedule_after_callback(void (*fn)(void*), void* userdata,
                                       int64_t delay_us, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    auto* timer = new uv_timer_t;
    int r = uv_timer_init(&self->loop_, timer);
    if (r != 0) {
      delete timer;
      return nullptr;
    }
    auto* box = new TimerCallbackBox{fn, userdata, self};
    timer->data = box;
    const uint64_t timeout_ms =
        delay_us <= 0 ? 1 : static_cast<uint64_t>((delay_us + 999) / 1000);
    r = uv_timer_start(timer, &timer_callback, timeout_ms, 0);
    if (r != 0) {
      // The handle is already in the loop's handle queue: it must be closed
      // (and freed by the close callback), not deleted directly.
      self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
      uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_callback);
      return nullptr;
    }
    {
      std::lock_guard lock(self->mu_);
      self->live_timers_.insert(timer);
    }
    return timer;
  }

  static void timer_callback(uv_timer_t* timer) {
    auto* box = static_cast<TimerCallbackBox*>(timer->data);
    auto* self = box->self;
    {
      std::lock_guard lock(self->mu_);
      self->live_timers_.erase(timer);
    }
    box->fn(box->userdata);
    // Never delete a libuv handle directly: it stays in the loop's handle
    // queue until uv_close runs its close callback.
    self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_callback);
  }

  static void timer_close_callback(uv_handle_t* handle) {
    auto* timer = reinterpret_cast<uv_timer_t*>(handle);
    auto* box = static_cast<TimerCallbackBox*>(timer->data);
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

  static void cancel_after_callback(void* timer_handle, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    if (!self->running_.load(std::memory_order_acquire)) {
      return;  // worker is stopping; timers are being cleaned up
    }
    auto* timer = static_cast<uv_timer_t*>(timer_handle);
    {
      std::lock_guard lock(self->mu_);
      if (self->live_timers_.find(timer) == self->live_timers_.end()) {
        return;  // already fired or unknown: safe no-op
      }
      self->pending_.push(
          {&cancel_timer_trampoline, new CancelTimerTask{self, timer}});
    }
    uv_async_send(&self->async_);
  }

  static void cancel_timer_trampoline(void* p) {
    auto task = std::unique_ptr<CancelTimerTask>(static_cast<CancelTimerTask*>(p));
    auto* self = task->self;
    auto* timer = task->timer;
    {
      std::lock_guard lock(self->mu_);
      if (self->live_timers_.erase(timer) == 0) {
        return;  // timer callback already fired and freed it
      }
    }
    uv_timer_stop(timer);
    self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_callback);
  }

  static void stop_all_timers_trampoline(void* p) {
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
      uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_callback);
    }
    // stop_all_done_ is set by timer_close_callback once every handle has
    // actually been closed (so uv_run can safely exit afterwards).
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
  std::unordered_set<uv_timer_t*> live_timers_;
  std::thread thread_;
  uint32_t foreign_id_{0};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_all_requested_{false};
  std::atomic<bool> stop_all_done_{false};
  std::atomic<int> pending_closes_{0};
};

}  // namespace demo
