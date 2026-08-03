#pragma once

#include <stdexec/execution.hpp>
#include <stdexec/stop_token.hpp>

#include <asio/basic_waitable_timer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace dcb {

// Scheduler that executes std::exec work on an Asio executor.
//
// std::exec (stdexec) scheduler: `stdexec::schedule(*sched)` returns a sender
// whose operation state posts its completion onto the wrapped Asio executor,
// so every step of a sender chain launched with dcb::on_scheduler() runs on
// that executor's thread(s).
//
// Two instantiations are used by the runtime:
//   - IoContextScheduler: wraps the io_context executor (single-threaded
//     event loop). Also supports schedule_at (timers) and the io-thread
//     identity tracking used by sync_wait's deadlock guard.
//   - PoolScheduler: wraps the thread_pool executor (blocking work).
//
// Lifetime: pending schedule operations capture `this` (the executor copy),
// so the scheduler must outlive the executor's execution. Runtime guarantees
// this: it stops and joins its threads before the scheduler is destroyed.
template <typename Executor>
class AsioScheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;
  using executor_type = Executor;

  explicit AsioScheduler(Executor ex)
    : ex_(std::move(ex)), state_(std::make_shared<State>()) {}

  // -------------------------------------------------------------------------
  // Sender returned by schedule(): completes on the executor via post().
  // -------------------------------------------------------------------------
  struct schedule_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    // Attributes exposed via get_env(): algorithms probe the completion
    // behavior of their child sender.
    struct attrs {
      constexpr auto query(
          stdexec::__get_completion_behavior_t<stdexec::set_value_t>) const noexcept {
        return stdexec::__completion_behavior::__inline_completion;
      }
      constexpr auto operator==(const attrs&) const noexcept -> bool = default;
    };

    static constexpr auto get_env() noexcept -> attrs { return {}; }

    const AsioScheduler* sched_;

    template <stdexec::receiver Rcvr>
    struct opstate {
      using operation_state_concept = stdexec::operation_state_tag;

      const AsioScheduler* sched_;
      Rcvr rcvr_;

      void start() noexcept {
        // `this` (the opstate) is kept alive by the caller until the
        // completion signal fires (P2300 guarantee), so capturing it in the
        // posted lambda is safe.
        try {
          asio::post(sched_->ex_, [op = this]() {
            stdexec::set_value(std::move(op->rcvr_));
          });
        } catch (...) {
          // asio::post only throws on allocation failure; deliver it as an
          // error rather than letting the exception escape start().
          stdexec::set_error(std::move(rcvr_),
                             std::make_exception_ptr(std::bad_alloc()));
        }
      }
    };

    template <stdexec::receiver Rcvr>
    opstate<Rcvr> connect(Rcvr rcvr) && {
      return opstate<Rcvr>{sched_, std::move(rcvr)};
    }
  };

  stdexec::sender auto schedule() const noexcept { return schedule_sender{this}; }

  const Executor& executor() const noexcept { return ex_; }

  // True only when the calling thread is the thread that runs the io_context.
  // Used by dcb::sync_wait as a deadlock guard — it refuses to block a thread
  // that the awaited sender depends on. Only meaningful for IoContextScheduler
  // (pool schedulers never have the io thread id set, so this stays false).
  bool current_thread_is_io() const noexcept {
    return std::this_thread::get_id() ==
           state_->io_thread_id.load(std::memory_order_acquire);
  }

  // Called by the owner (Runtime) right after it spawns the io thread.
  void set_io_thread_id(std::thread::id id) {
    state_->io_thread_id.store(id, std::memory_order_release);
  }

  // Two schedulers are equal when they wrap the same executor.
  bool operator==(const AsioScheduler& o) const noexcept {
    return ex_ == o.ex_ && state_ == o.state_;
  }

 private:
  // Shared mutable state: keeps the scheduler copyable (std::atomic is
  // non-copyable, and the scheduler concept requires copy constructibility).
  struct State {
    std::atomic<std::thread::id> io_thread_id{};
  };

  Executor ex_;
  std::shared_ptr<State> state_;
};

/// Scheduler backed by the thread_pool executor (blocking work).
using PoolScheduler = AsioScheduler<asio::thread_pool::executor_type>;

/// Scheduler backed by the io_context executor (single-threaded event loop).
/// Additionally provides schedule_at (timers) on the io_context.
class IoContextScheduler : public AsioScheduler<asio::io_context::executor_type> {
 public:
  using base = AsioScheduler<asio::io_context::executor_type>;

  explicit IoContextScheduler(asio::io_context& ioc)
    : base(ioc.get_executor()), ioc_(ioc) {}

  asio::io_context& io() const noexcept { return ioc_; }

  // -------------------------------------------------------------------------
  // Sender returned by schedule_at(): completes after `dur` on the io thread.
  // Cancellation: if the connected receiver's environment provides an
  // inplace_stop_token, a stop request cancels the timer and completes with
  // set_stopped().
  // -------------------------------------------------------------------------
  template <typename Duration>
  struct schedule_at_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
      stdexec::set_value_t(),
      stdexec::set_error_t(std::exception_ptr),
      stdexec::set_stopped_t()>;

    const IoContextScheduler* sched_;
    Duration dur_;

    // Stop callback that cancels the shared timer. Runs on the thread that
    // requests the stop; asio timer cancellation is thread-safe.
    struct cancel_timer_fn {
      std::shared_ptr<asio::steady_timer> timer;
      void operator()() const noexcept {
        try {
          timer->cancel();
        } catch (...) {
        }
      }
    };

    template <stdexec::receiver Rcvr>
    struct opstate {
      using operation_state_concept = stdexec::operation_state_tag;

      const IoContextScheduler* sched_;
      Duration dur_;
      std::shared_ptr<asio::steady_timer> timer_;
      Rcvr rcvr_;
      std::optional<stdexec::inplace_stop_callback<cancel_timer_fn>> stop_cb_;

      opstate(const IoContextScheduler* sched, Duration dur, Rcvr rcvr)
        : sched_(sched), dur_(dur), rcvr_(std::move(rcvr)) {}

      void start() noexcept
      {
        // The opstate must outlive the completion (P2300 guarantee); the
        // timer object is kept alive by shared_ptr so a cancellation or io
        // shutdown never touches freed memory.
        timer_ = std::make_shared<asio::steady_timer>(sched_->ioc_, dur_);
        auto timer = timer_;
        auto* op = this;

        // Honour an inplace_stop_token from the receiver's environment:
        // request_stop() cancels the timer and completes with set_stopped().
        if constexpr (stdexec::__callable<stdexec::get_stop_token_t,
                                          decltype(stdexec::get_env(rcvr_))>) {
          auto token = stdexec::get_stop_token(stdexec::get_env(rcvr_));
          if constexpr (std::same_as<std::decay_t<decltype(token)>,
                                     stdexec::inplace_stop_token>) {
            if (token.stop_requested()) {
              stdexec::set_stopped(std::move(rcvr_));
              return;
            }
            op->stop_cb_.emplace(token, cancel_timer_fn{timer});
          }
        }

        try {
          timer->async_wait([op, timer](const asio::error_code& ec) {
            (void)timer;  // keep the timer alive until the callback runs
            if (ec == asio::error::operation_aborted) {
              // Cancelled via the stop token: complete stopped.
              stdexec::set_stopped(std::move(op->rcvr_));
              return;
            }
            if (ec) {
              stdexec::set_error(
                  std::move(op->rcvr_),
                  std::make_exception_ptr(std::runtime_error(ec.message())));
              return;
            }
            stdexec::set_value(std::move(op->rcvr_));
          });
        } catch (...) {
          stdexec::set_error(std::move(rcvr_),
                             std::make_exception_ptr(std::bad_alloc()));
        }
      }
    };

    template <stdexec::receiver Rcvr>
    schedule_at_sender::opstate<Rcvr> connect(Rcvr rcvr) && {
      return schedule_at_sender::opstate<Rcvr>{sched_, dur_, std::move(rcvr)};
    }
  };

  template <typename Rep, typename Period>
  stdexec::sender auto schedule_at(std::chrono::duration<Rep, Period> dur) const noexcept {
    return schedule_at_sender<std::chrono::duration<Rep, Period>>{this, dur};
  }

 private:
  asio::io_context& ioc_;
};

// Block the calling thread until `sndr` completes; returns the value or
// rethrows the sender's error. NEVER call on the io thread (self-deadlock).
// Implemented in runtime.hpp where the runtime singleton is available.
template <stdexec::sender S>
auto sync_wait(S&& sndr);

}  // namespace dcb
