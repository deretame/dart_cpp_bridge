#pragma once

// ForeignExecutor — Generic external-runtime executor.
//
// Forwards async_simple::Executor::schedule() to a user-registered C callback.
// Any event loop only needs to implement "execute a void(*)(void*) on the loop thread"
// to plug into the bridge's channel / coroutine system.
// Optionally, the runtime can also register native timer callbacks
// (dcb_schedule_after_fn / dcb_cancel_after_fn) so co_await
// async_simple::coro::sleep(...) uses a real event-loop timer instead of the
// thread-based fallback.
//
// No dependency on asio. Depends on async_simple/Executor.h and the
// project's own foreign_runtime.h (for the optional timer callback types).
// See docs/foreign_runtime_design.md for details.

#include <async_simple/Executor.h>
#include <async_simple/Signal.h>

#include "dart_cpp_bridge/foreign_runtime.h"

#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace dcb {

class ForeignExecutor : public async_simple::Executor {
 public:
  using ScheduleFn = void (*)(void (*fn)(void*), void* userdata, void* ctx);
  using ScheduleAfterFn = dcb_schedule_after_fn;
  using CancelAfterFn = dcb_cancel_after_fn;

  /// No native timer support (sleep uses the thread-based fallback).
  ForeignExecutor(std::string name, ScheduleFn schedule_fn, void* ctx)
      : ForeignExecutor(std::move(name), schedule_fn, nullptr, nullptr, ctx) {}

  /// Optional native timer support: when both [schedule_after_fn] and
  /// [cancel_after_fn] are non-null, co_await sleep(...) is driven by the
  /// foreign runtime's own timer; otherwise the thread fallback is used.
  ForeignExecutor(std::string name, ScheduleFn schedule_fn,
                  ScheduleAfterFn schedule_after_fn,
                  CancelAfterFn cancel_after_fn, void* ctx)
      : name_(std::move(name)),
        schedule_fn_(schedule_fn),
        schedule_after_fn_(schedule_after_fn),
        cancel_after_fn_(cancel_after_fn),
        ctx_(ctx) {}

  ~ForeignExecutor() override {
    alive_.store(false, std::memory_order_release);
    if (timer_state_) {
      timer_state_->alive.store(false, std::memory_order_release);
    }
  }

  ForeignExecutor(const ForeignExecutor&) = delete;
  ForeignExecutor& operator=(const ForeignExecutor&) = delete;

  // ─── async_simple::Executor interface ─────────────────────────────────────

  /// Post a task to the external runtime.
  /// Boxes the std::function on the heap and passes it via the C function pointer.
  /// The external loop thread frees and invokes the original function inside the trampoline.
  bool schedule(Func func) override {
    if (!alive_.load(std::memory_order_acquire)) return false;
    auto* boxed = new Func(std::move(func));
    schedule_fn_(&trampoline, boxed, ctx_);
    return true;
  }

  bool schedule(Func func, uint64_t /*schedule_info*/) override {
    return schedule(std::move(func));
  }

  /// Return whether the current thread is the external loop thread.
  /// async_simple calls this internally in many places (syncAwait assertion, FutureState scheduling, etc.).
  /// The base class default throws, so this must be overridden.
  bool currentThreadInExecutor() const override {
    auto id = loop_thread_id_.load(std::memory_order_acquire);
    return id != std::thread::id{} && std::this_thread::get_id() == id;
  }

  async_simple::ExecutorStat stat() const override {
    async_simple::ExecutorStat s;
    return s;
  }

  async_simple::IOExecutor* getIOExecutor() override { return nullptr; }

  // ─── ForeignExecutor-specific interface ─────────────────────────────────────

  const std::string& foreign_name() const { return name_; }
  bool alive() const { return alive_.load(std::memory_order_acquire); }

  /// Mark as inactive (called on unregister). Subsequent schedule calls return false.
  void deactivate() {
    alive_.store(false, std::memory_order_release);
    if (timer_state_) {
      timer_state_->alive.store(false, std::memory_order_release);
    }
  }

  /// Set the external loop thread ID (call after the loop thread starts).
  /// Enables currentThreadInExecutor() to work correctly.
  void set_loop_thread_id(std::thread::id id) {
    loop_thread_id_.store(id, std::memory_order_release);
  }

 protected:
  // Timer schedule used by executor->after() / async_simple::coro::sleep.
  //
  // Preferred path: the foreign runtime's native timer (when registered).
  // Fallback: a detached waiter thread. Both paths support cancellation —
  // a SignalType::Terminate handler wakes the sleeper early and
  // TimeAwaiter::await_resume() then throws SignalException — and neither
  // touches `this` after the executor is deactivated/destroyed.
  void schedule(Func func, Duration dur, uint64_t /*schedule_info*/,
                async_simple::Slot* slot) override {
    if (!alive_.load(std::memory_order_acquire) ||
        !timer_state_->alive.load(std::memory_order_acquire)) {
      return;
    }
    if (schedule_after_fn_ != nullptr && cancel_after_fn_ != nullptr &&
        schedule_native_timer(std::move(func), dur, slot)) {
      return;
    }
    schedule_thread_fallback(std::move(func), dur, slot);
  }

 private:
  /// Boxed job shared by the native timer callback and the cancellation
  /// handler; the atomic guarantees the resume runs exactly once even when
  /// the timer firing and the Terminate signal race.
  struct TimerJob {
    explicit TimerJob(Func f) : func(std::move(f)) {}
    Func func;
    std::atomic<bool> consumed{false};
  };
  struct TimerBox {
    std::shared_ptr<TimerJob> job;
  };

  static void timer_trampoline(void* p) {
    auto box = std::unique_ptr<TimerBox>(static_cast<TimerBox*>(p));
    if (!box->job->consumed.exchange(true, std::memory_order_acq_rel)) {
      box->job->func();
    }
  }

  bool schedule_native_timer(Func func, Duration dur,
                             async_simple::Slot* slot) {
    auto job = std::make_shared<TimerJob>(std::move(func));
    auto* box = new TimerBox{job};
    void* handle = schedule_after_fn_(&ForeignExecutor::timer_trampoline, box,
                                      static_cast<int64_t>(dur.count()), ctx_);
    if (handle == nullptr) {
      // Contract: on failure fn is not called and userdata must not be used.
      delete box;
      func = std::move(job->func);
      return false;
    }
    if (slot != nullptr) {
      const bool registered =
          async_simple::signalHelper{async_simple::SignalType::Terminate}
              .tryEmplace(
                  slot, [this, handle, job](async_simple::SignalType,
                                            async_simple::Signal*) {
                    // Runs on the thread that emits Terminate; both C
                    // callbacks are thread-safe by contract.
                    cancel_after_fn_(handle, ctx_);
                    // Do NOT consume the job here: the timer may already have
                    // fired (its trampoline then ran the func, and the posted
                    // trampoline below simply skips). The CAS in
                    // timer_trampoline guarantees exactly-once execution.
                    schedule_fn_(&ForeignExecutor::timer_trampoline,
                                 new TimerBox{job}, ctx_);
                  });
      if (!registered) {
        // Cancelled between TimeAwaiter::await_ready() and tryEmplace():
        // stop the timer and resume immediately; await_resume() throws.
        cancel_after_fn_(handle, ctx_);
        schedule_fn_(&ForeignExecutor::timer_trampoline, new TimerBox{job},
                     ctx_);
      }
    }
    return true;
  }

  void schedule_thread_fallback(Func func, Duration dur,
                                async_simple::Slot* slot) {
    auto state = timer_state_;
    auto schedule_fn = schedule_fn_;
    auto ctx = ctx_;
    std::thread([state, schedule_fn, ctx, func = std::move(func), dur, slot]() {
      try {
        auto promise = std::make_unique<std::promise<void>>();
        auto future = promise->get_future();
        const bool registered =
            async_simple::signalHelper{async_simple::SignalType::Terminate}
                .tryEmplace(slot, [p = std::move(promise)](
                                      async_simple::SignalType,
                                      async_simple::Signal*) { p->set_value(); });
        if (registered) {
          // Wait until the duration elapses or the Terminate signal fires
          // (the handler above sets the promise and wakes us early).
          future.wait_for(dur);
        }
        if (!state->alive.load(std::memory_order_acquire)) {
          return;  // executor deactivated/destroyed while waiting
        }
        auto* boxed = new Func(std::move(func));
        schedule_fn(&ForeignExecutor::trampoline, boxed, ctx);
      } catch (const std::exception& e) {
        std::cerr << "[dcb] ForeignExecutor: timer task failed: " << e.what()
                  << std::endl;
      } catch (...) {
        std::cerr
            << "[dcb] ForeignExecutor: timer task failed: unknown exception"
            << std::endl;
      }
    }).detach();
  }

  /// Trampoline: runs on the external loop thread, frees the heap-allocated Func and invokes it.
  static void trampoline(void* p) {
    auto f = std::unique_ptr<Func>(static_cast<Func*>(p));
    (*f)();
  }

  // Shared with timer waiter threads so they can check liveness without
  // touching `this` after the executor is deactivated/destroyed.
  struct TimerState {
    std::atomic<bool> alive{true};
  };

  std::string name_;
  ScheduleFn schedule_fn_;
  ScheduleAfterFn schedule_after_fn_{nullptr};
  CancelAfterFn cancel_after_fn_{nullptr};
  void* ctx_;
  std::atomic<bool> alive_{true};
  std::atomic<std::thread::id> loop_thread_id_{};
  std::shared_ptr<TimerState> timer_state_ = std::make_shared<TimerState>();
};

}  // namespace dcb
