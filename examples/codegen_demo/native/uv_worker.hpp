#pragma once

// UvWorker — Independent runtime based on libuv, exposing a stdexec scheduler.
//
// Demonstrates how a non-asio event loop participates in the sender world
// (docs/cpp26_executor_model_usage.md §7): the worker owns a uv_loop_t + a
// dedicated thread and hands out a UvScheduler. Business code runs work on
// the uv loop thread with the standard algorithms:
//
//   auto& worker = ...;
//   auto s = stdexec::starts_on(worker.scheduler(), stdexec::then(f));  // on uv thread
//   auto t = worker.scheduler().schedule_after(100ms);                  // uv timer
//   auto w = worker.scheduler().uv_work([] { return heavy(); });        // pool + uv
//
// Wake-up path: schedule() enqueues a task and uv_async_send()s the loop;
// the loop thread drains the queue and runs the tasks.

#include <uv.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "uv_scheduler.hpp"

namespace demo {

class UvWorker {
 public:
  explicit UvWorker(std::string name) : name_(std::move(name)) {}

  ~UvWorker() { stop(); }

  UvWorker(const UvWorker&) = delete;
  UvWorker& operator=(const UvWorker&) = delete;

  void start() {
    if (running_.load(std::memory_order_acquire)) {
      return;
    }

    uv_loop_init(&loop_);
    st_ = std::make_shared<UvSchedState>();
    st_->loop = &loop_;
    // uv_async_init before the loop thread starts: no other thread can race
    // us while the loop is not running.
    uv_async_init(&loop_, &st_->async, &UvSchedState::on_async);
    st_->async.data = st_.get();
    st_->async_ready.store(true, std::memory_order_release);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { uv_run(&loop_, UV_RUN_DEFAULT); });
  }

  void stop() {
    if (!running_.load(std::memory_order_acquire)) {
      return;
    }
    running_.store(false, std::memory_order_release);

    // Reject new work, then wake the loop so the pending drain runs.
    {
      std::lock_guard lock(st_->mu);
      st_->closed = true;
    }
    st_->wake();

    // Let the loop drain the start queue and finish in-flight uv_queue_work
    // operations before stopping it: closing a loop with an in-flight
    // request is illegal, and after_work needs a running loop to release the
    // WorkState self-hold.
    while (true) {
      bool queue_empty;
      {
        std::lock_guard lock(st_->mu);
        queue_empty = st_->head == nullptr;
      }
      if (queue_empty &&
          st_->work_in_flight.load(std::memory_order_acquire) == 0) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    uv_stop(&loop_);
    if (thread_.joinable()) {
      thread_.join();
    }

    // Close every remaining handle (async + any pending timers), let the loop
    // process the close callbacks, then close the loop. Timers preserve their
    // typed close callback (TimerState::on_close) so their shared state is
    // released exactly once.
    st_->async_ready.store(false, std::memory_order_release);
    uv_walk(&loop_,
            [](uv_handle_t* h, void*) {
              if (uv_is_closing(h)) {
                return;  // already closing; its close callback is pending
              }
              if (h->type == UV_TIMER && h->data) {
                auto* base = static_cast<uv_detail::TimerStateBase*>(h->data);
                uv_close(h, base->close_fn);
              } else {
                uv_close(h, nullptr);
              }
            },
            nullptr);
    uv_run(&loop_, UV_RUN_DEFAULT);
    if (uv_loop_close(&loop_) != 0) {
      std::fprintf(stderr, "[foreign_demo] uv_loop_close failed (leak)\n");
    }
    st_.reset();
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  const std::string& name() const { return name_; }

  /// The stdexec scheduler backed by this worker's uv loop.
  /// Must not outlive the worker (same rule as IoContextScheduler).
  UvScheduler scheduler() { return UvScheduler{st_}; }

 private:
  std::string name_;
  uv_loop_t loop_{};
  std::shared_ptr<UvSchedState> st_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace demo
