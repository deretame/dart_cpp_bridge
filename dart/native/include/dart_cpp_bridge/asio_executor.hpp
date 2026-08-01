#pragma once

#include <async_simple/Executor.h>
#include <async_simple/Signal.h>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/dispatch.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <utility>

namespace dcb {

// Schedule async_simple tasks onto asio::io_context (single-threaded OK).
//
// Lifetime: submitted tasks capture `this` (to maintain the pending-task
// counter), so the executor must outlive the io_context's execution. Runtime
// guarantees this: it stops and joins the io thread before the executor is
// destroyed.
class AsioExecutor : public async_simple::Executor {
 public:
  explicit AsioExecutor(asio::io_context& ioc) : ioc_(ioc) {}

  bool schedule(Func func) override {
    pending_tasks_.fetch_add(1, std::memory_order_relaxed);
    asio::post(ioc_, [this, fn = std::move(func)]() {
      fn();
      pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
    });
    return true;
  }

  bool schedule(Func func, uint64_t schedule_info) override {
    pending_tasks_.fetch_add(1, std::memory_order_relaxed);
    auto task = [this, fn = std::move(func)]() {
      fn();
      pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
    };
    if ((schedule_info & 0xF) >=
        static_cast<uint64_t>(async_simple::Executor::Priority::YIELD)) {
      asio::post(ioc_, std::move(task));
    } else {
      asio::dispatch(ioc_, std::move(task));
    }
    return true;
  }

  // Return a task to the io thread. `opts.prompt` selects immediate execution
  // (dispatch) vs deferred (post); the default (prompt=true) dispatches, which
  // matches the previous behavior. `ctx` is ignored: there is a single
  // io_context, so all contexts are equivalent.
  bool checkin(Func func, Context /*ctx*/,
               async_simple::ScheduleOptions opts) override {
    if (opts.prompt) {
      asio::dispatch(ioc_, std::move(func));
    } else {
      asio::post(ioc_, std::move(func));
    }
    return true;
  }

  void* checkout() override { return &ioc_; }

  // Snapshot of executor statistics. pendingTaskCount tracks work submitted
  // through the schedule() overloads above (the dominant path); checkin/timer
  // submissions are not counted, so treat it as a best-effort metric.
  async_simple::ExecutorStat stat() const override {
    async_simple::ExecutorStat s;
    s.pendingTaskCount = pending_tasks_.load(std::memory_order_relaxed);
    return s;
  }

  // We do not expose a separate IOExecutor: the io_context itself drives all
  // IO. Returning nullptr is the documented way to say "not provided".
  async_simple::IOExecutor* getIOExecutor() override { return nullptr; }

  // True only when the calling thread is the io thread that runs the
  // io_context. async_simple uses this as a deadlock guard — e.g. syncAwait
  // refuses to block a thread that the awaited Lazy depends on. Returning the
  // real answer (instead of a conservative constant `true`) is what makes
  // syncAwait usable from ordinary non-io threads while still rejecting it on
  // the io thread.
  bool currentThreadInExecutor() const override {
    return std::this_thread::get_id() == io_thread_id_.load(std::memory_order_acquire);
  }

  size_t currentContextId() const override {
    return reinterpret_cast<size_t>(&ioc_);
  }

  // Called by the owner (Runtime) right after it spawns the io thread, so that
  // currentThreadInExecutor() can tell the io thread apart from other callers.
  void set_io_thread_id(std::thread::id id) {
    io_thread_id_.store(id, std::memory_order_release);
  }

 protected:
  // Timer schedule used by executor->after() / async_simple::coro::sleep. Runs
  // `func` on the io thread after `dur` via a steady_timer, so no thread
  // blocks while waiting (unlike the base implementation, which spawns a
  // thread and sleeps on it). The 2-arg schedule(Func, Duration) inherited
  // from the base delegates here, so both paths use the timer.
  //
  // When `slot` is non-null (async_simple::coro::sleep inherits the Slot from
  // the coroutine's signal chain, e.g. via Lazy::setLazyLocal), a
  // SignalType::Terminate handler is registered that cancels the timer.
  // TimeAwaiter::await_resume() then sees the fired signal and throws
  // SignalException, so `co_await async_simple::coro::sleep(...)` is
  // interruptible by cancellation. The handler is consumed/cleared by
  // TimeAwaiter's checkHasCanceled, so no explicit cleanup is needed here.
  //
  // `schedule_info` (priority) is ignored: asio timers fire on the io thread
  // regardless.
  void schedule(Func func, Duration dur, uint64_t /*schedule_info*/,
                async_simple::Slot* slot) override {
    auto timer = std::make_shared<asio::steady_timer>(ioc_, dur);
    if (slot != nullptr) {
      const bool registered =
          async_simple::signalHelper{async_simple::SignalType::Terminate}
              .tryEmplace(slot, [timer](async_simple::SignalType,
                                        async_simple::Signal*) {
                // Runs on the thread that emits the signal (e.g. a Dart
                // caller thread); asio timer cancellation is thread-safe.
                try {
                  timer->cancel();
                } catch (const std::exception& e) {
                  // Signal handlers run inside Signal::emits() (noexcept).
                  // A failed cancel is not fatal: the timer fires normally and
                  // TimeAwaiter::await_resume() still throws, but log it so
                  // the failure is not invisible.
                  std::cerr << "[dcb] AsioExecutor: failed to cancel sleep "
                               "timer from signal handler: "
                            << e.what() << std::endl;
                } catch (...) {
                  std::cerr << "[dcb] AsioExecutor: failed to cancel sleep "
                               "timer from signal handler: unknown exception"
                            << std::endl;
                }
              });
      if (!registered) {
        // The signal fired between TimeAwaiter::await_ready() and our
        // emplace. Cancel immediately; the pending handler below resumes the
        // coroutine and await_resume() throws SignalException.
        try {
          timer->cancel();
        } catch (const std::exception& e) {
          std::cerr << "[dcb] AsioExecutor: failed to cancel sleep timer "
                       "(signal fired before emplace): "
                    << e.what() << std::endl;
        } catch (...) {
          std::cerr << "[dcb] AsioExecutor: failed to cancel sleep timer "
                       "(signal fired before emplace): unknown exception"
                    << std::endl;
        }
      }
    }
    timer->async_wait([fn = std::move(func), timer](const asio::error_code&) {
      // Resume even when the timer was cancelled (operation_aborted):
      // TimeAwaiter::await_resume() decides whether to throw.
      fn();
    });
  }

 private:
  asio::io_context& ioc_;
  std::atomic<std::thread::id> io_thread_id_{};
  std::atomic<size_t> pending_tasks_{0};
};

}  // namespace dcb
