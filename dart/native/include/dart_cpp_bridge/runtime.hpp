#pragma once

#include "dart_cpp_bridge/channel.hpp"

#include <stdexec/execution.hpp>

#include <exec/asio/asio_config.hpp>
#include <exec/asio/asio_thread_pool.hpp>
#include <exec/asio/use_sender.hpp>

// asio namespace switch: standalone asio (default) or boost::asio (when
// DCB_USE_BOOST_ASIO is defined, e.g. via the CMake option of the same name).
// Business code should use DCB_ASIO_NS instead of a hard-coded asio::.
#if defined(DCB_USE_BOOST_ASIO)
#  include <boost/asio/io_context.hpp>
#  include <boost/asio/post.hpp>
#  include <boost/asio/steady_timer.hpp>
#  include <boost/asio/executor_work_guard.hpp>
#  define DCB_ASIO_NS boost::asio
#else
#  include <asio/io_context.hpp>
#  include <asio/post.hpp>
#  include <asio/steady_timer.hpp>
#  include <asio/executor_work_guard.hpp>
#  define DCB_ASIO_NS asio
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace dcb {

// Marker type used to bridge void values through senders that require a
// movable payload (e.g. the co::oneshot channel). `co_await
// dcb::spawn_blocking(f)` for a void `f` completes with dcb::Unit.
struct Unit {};

// P2300 (std::execution / stdexec) scheduler adapter that runs sender work on
// an asio::io_context event loop — one thread by default, with an optional
// configurable number of runner threads. Design
// reference: docs/cpp26_executor_model_usage.md §12.6 (Asio adapter).
//
// `stdexec::schedule(*sched)` returns the official exec::asio adapter sender
// for `asio::post(io_context, use_sender)`: the posted handler runs on one of
// the scheduler runner threads, so every step of a chain launched with
// `stdexec::starts_on(*sched, ...)` executes on a scheduler thread, and its
// completion fires back on a scheduler thread. Error/stopped mapping follows the
// adapter contract: operation_aborted / operation_canceled -> set_stopped,
// other error_code -> set_error(std::exception_ptr).
//
// Timers (dcb::sleep) use the same adapter via asio::steady_timer +
// async_wait(use_sender), so sleeping never blocks the io thread and a stop
// request cancels the timer.
//
// Lifetime: pending schedule operations capture the io_context reference, so
// the scheduler must not outlive the io_context. The Runtime owns both and
// guarantees the order: io_context member declared before the scheduler
// member; stop() joins all io threads before destruction.
//
// The scheduler is trivially copyable-equivalent in the sense required by the
// stdexec scheduler concept: copies share the underlying io_context pointer.
// current_thread_is_io() delegates to asio's own
// executor_type::running_in_this_thread(), so no thread-id bookkeeping is
// needed (it backs dcb::sync_wait's self-deadlock guard for every runner
// thread).
class IoContextScheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;

  explicit IoContextScheduler(DCB_ASIO_NS::io_context& ioc) : ioc_(&ioc) {}

  stdexec::sender auto schedule() const noexcept {
    // asio::post + use_sender never fails and is not cancellable, but the
    // exec::asio adapter still *declares* set_error / set_stopped. stdexec::task
    // requires its start scheduler to be infallible (the default task_scheduler
    // cannot be constructed from a scheduler that may send errors), so map the
    // (never-firing) error/stopped branches to set_value() here.
    return exec::asio::asio_impl::post(*ioc_, exec::asio::use_sender)
         | stdexec::upon_error([](std::exception_ptr) noexcept {})
         | stdexec::upon_stopped([]() noexcept {});
  }

  // Timed scheduling on the io_context: asio::steady_timer +
  // async_wait(use_sender). This is what makes the scheduler usable as a
  // timed scheduler (schedule_after) by dcb::sleep / co::stream::interval.
  // Completion: set_value_t() on the io thread; cancellation via the
  // standard stop_token machinery (stop cancels the timer, set_stopped).
  template <typename Rep, typename Period>
  stdexec::sender auto schedule_after(std::chrono::duration<Rep, Period> dur) const noexcept {
    // The timer must outlive the pending async operation: the shared_ptr is
    // held by the then-step until the pipeline completes (or is cancelled).
    auto timer = std::make_shared<DCB_ASIO_NS::steady_timer>(*ioc_);
    timer->expires_after(dur);
    return timer->async_wait(exec::asio::use_sender)
         | stdexec::then([timer] { (void)timer; });
  }

  DCB_ASIO_NS::io_context& io() const noexcept { return *ioc_; }

  DCB_ASIO_NS::io_context::executor_type executor() const noexcept {
    return ioc_->get_executor();
  }

  /// True only when the calling thread runs the io_context. Used by
  /// dcb::sync_wait to reject calls that would self-deadlock.
  bool current_thread_is_io() const noexcept {
    return ioc_->get_executor().running_in_this_thread();
  }

  // Two schedulers are equal when they wrap the same io_context.
  friend bool operator==(const IoContextScheduler&, const IoContextScheduler&) = default;

 private:
  DCB_ASIO_NS::io_context* ioc_;
};

using DartPostFn = void (*)(std::int64_t port, const std::uint8_t* data, std::size_t len,
                            void* userdata);

class Runtime {
 public:
  static Runtime& instance();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  void start();
  void stop();
  bool running() const { return started_.load(std::memory_order_acquire); }

  /// Run [fn] while the runtime's start/stop gate is held. This is the
  /// acceptance boundary for external callers that need to submit work
  /// without racing Runtime::stop() between the running check and submission.
  /// Returns false when the runtime is already stopped.
  template <typename Fn>
  bool try_accept(Fn&& fn) {
    std::lock_guard lock(start_stop_mu_);
    if (!started_.load(std::memory_order_acquire)) {
      return false;
    }
    std::forward<Fn>(fn)();
    return true;
  }

  /// Set blocking thread pool size (must be called before start; default 4).
  void set_pool_threads(std::uint32_t n) {
    std::lock_guard lock(start_stop_mu_);
    if (lifecycle_ != Lifecycle::kStopped) {
      return;
    }
    pool_threads_ = n ? n : 1;
  }

  /// Set the number of threads that run the io scheduler (must be called
  /// before start; default 1). A value of zero is normalized to one.
  void set_io_threads(std::uint32_t n) {
    std::lock_guard lock(start_stop_mu_);
    if (lifecycle_ != Lifecycle::kStopped) {
      return;
    }
    io_threads_count_ = n ? n : 1;
  }

  void ensure_running() {
    if (!running()) {
      throw std::runtime_error("runtime stopped");
    }
  }

  DCB_ASIO_NS::io_context& io() { return io_; }

  /// The blocking thread pool executor (asio interop: asio::post(rt.pool(), ...)).
  DCB_ASIO_NS::thread_pool::executor_type pool() { return pool_->get_executor(); }

  /// The scheduler that runs on the io_context event loop. It uses one runner
  /// thread by default; configure more with set_io_threads() before start().
  /// Business senders are launched here (stdexec::starts_on(*io_scheduler(),
  /// sndr)); completions are delivered on a scheduler runner thread.
  IoContextScheduler* io_scheduler() { return &io_sched_; }

  /// The scheduler backed by the blocking thread pool (official
  /// exec::asio::asio_thread_pool adapter; spawn_blocking work).
  auto blocking_scheduler() { return pool_->get_scheduler(); }

  void set_dart_post(DartPostFn fn, void* userdata) {
    std::lock_guard lock(post_mu_);
    post_fn_ = fn;
    post_userdata_ = userdata;
  }

  void post_to_dart(std::int64_t port, const std::uint8_t* data, std::size_t len) {
    if (!running()) {
      return;
    }
    DartPostFn fn = nullptr;
    void* userdata = nullptr;
    {
      std::lock_guard lock(post_mu_);
      fn = post_fn_;
      userdata = post_userdata_;
    }
    if (fn) {
      fn(port, data, len, userdata);
    }
  }

 private:
  enum class Lifecycle { kStopped, kStarting, kRunning, kStopping };

  Runtime();
  ~Runtime();

  DCB_ASIO_NS::io_context io_;
  IoContextScheduler io_sched_{io_};
  std::unique_ptr<exec::asio::asio_thread_pool> pool_;
  std::unique_ptr<DCB_ASIO_NS::executor_work_guard<DCB_ASIO_NS::io_context::executor_type>> guard_;
  std::vector<std::thread> io_threads_;
  std::atomic<bool> started_{false};
  Lifecycle lifecycle_{Lifecycle::kStopped};
  std::uint32_t pool_threads_{4};
  std::uint32_t io_threads_count_{1};
  DartPostFn post_fn_{nullptr};
  void* post_userdata_{nullptr};
  mutable std::mutex post_mu_;
  std::mutex start_stop_mu_;
};

// ---------------------------------------------------------------------------
// Scheduler plumbing (std::exec style)
// ---------------------------------------------------------------------------

namespace detail {

// Environment that exposes a scheduler as both get_scheduler and
// get_start_scheduler. Senders that depend on a scheduler (e.g. stdexec::task)
// need it to compute completion signatures and to run.
template <typename Sched>
struct sched_env {
  const Sched* sched;

  constexpr auto query(stdexec::get_scheduler_t) const noexcept -> const Sched& {
    return *sched;
  }
  constexpr auto query(stdexec::get_start_scheduler_t) const noexcept -> const Sched& {
    return *sched;
  }
};

// The default scheduler for spawn_blocking: the runtime's blocking pool.
// Lives in a function (rather than inline in the default argument) so the
// stopped-runtime guard runs before the pool is dereferenced.
inline auto default_blocking_scheduler() {
  auto& rt = Runtime::instance();
  rt.ensure_running();
  return rt.blocking_scheduler();
}

}  // namespace detail

// Block the calling thread until `sndr` completes. Returns the value or
// rethrows the sender's error (mirrors std::exec's sync_wait semantics, with
// a runtime deadlock guard):
//
//   int v = std::get<0>(*dcb::sync_wait(stdexec::starts_on(
//       *Runtime::instance().io_scheduler(), add(a, b))));
//
// NEVER call on the io thread: blocking the io thread while the awaited
// sender needs it is a self-deadlock. IoContextScheduler::current_thread_is_io()
// identifies the io thread and this wrapper rejects it with std::logic_error.
template <stdexec::sender S>
auto sync_wait(S&& sndr) {
  auto* sched = Runtime::instance().io_scheduler();
  if (sched && sched->current_thread_is_io()) {
    throw std::logic_error(
      "dcb::sync_wait must not be called on the io thread (self-deadlock)");
  }
  return stdexec::sync_wait(std::forward<S>(sndr));
}

// Run a blocking callable on the given scheduler (default: the runtime's
// blocking thread pool) and return a sender that resolves to its result.
// The io thread is never blocked: the callable runs on a pool thread
// (starts_on the scheduler) while the awaiting coroutine suspends.
// Completion signatures: set_value_t(WireT) (WireT = T, or dcb::Unit for
// void) / set_error_t(std::exception_ptr), always delivered on the io thread
// (continues_on the io scheduler).
//
//   // inside a coroutine running on io:
//   auto v = co_await dcb::spawn_blocking([&] { return heavyComputation(); });
//
//   // run on a different scheduler (any stdexec::scheduler):
//   auto v = co_await dcb::spawn_blocking(f, my_pool.get_scheduler());
//
//   // block a normal (non-io) thread for the result:
//   auto v = std::get<0>(*dcb::sync_wait(dcb::spawn_blocking(f)));
//
// Exceptions thrown by the callable are captured on the pool thread and
// rethrown at the awaiter (set_error).
template <class F,
          stdexec::scheduler Sched = decltype(detail::default_blocking_scheduler())>
auto spawn_blocking(F&& f, Sched sched = detail::default_blocking_scheduler())
    -> stdexec::sender auto {
  using T = std::invoke_result_t<std::decay_t<F>>;
  using WireT = std::conditional_t<std::is_void_v<T>, Unit, T>;
  auto& rt = Runtime::instance();
  rt.ensure_running();
  // std::exec style: schedule the callable onto the given scheduler, then
  // migrate the completion back to the io scheduler. Exceptions inside the
  // callable become set_error automatically.
  auto work = stdexec::just() | stdexec::then([f = std::forward<F>(f)]() mutable -> WireT {
    if constexpr (std::is_void_v<T>) {
      f();
      return Unit{};
    } else {
      return f();
    }
  });
  return stdexec::starts_on(std::move(sched), std::move(work))
       | stdexec::continues_on(*rt.io_scheduler());
}

// Sleep for `dur` on a timed scheduler. Defaults to the runtime's io
// scheduler (official exec::asio adapter: asio::steady_timer +
// async_wait(use_sender); the io thread stays responsive). Completion:
// set_value_t() on the scheduler's own thread. Supports cancellation through
// the standard stop_token machinery (a stop request cancels the timer and
// completes with set_stopped).
//
// Any scheduler providing `schedule_after(duration)` works, e.g. the
// libuv-backed UvScheduler in codegen_demo:
//
//   co_await dcb::sleep(100ms);              // runtime io thread
//   co_await dcb::sleep(100ms, uv_sched);    // custom timed scheduler
//
// Durations are truncated to milliseconds (the granularity every current
// timed scheduler implements).
template <typename Rep, typename Period,
          typename Sched = std::decay_t<decltype(*Runtime::instance().io_scheduler())>>
  requires requires(Sched s, std::chrono::milliseconds ms) { s.schedule_after(ms); }
stdexec::sender auto sleep(std::chrono::duration<Rep, Period> dur,
                           Sched sched = *Runtime::instance().io_scheduler()) {
  return sched.schedule_after(std::chrono::duration_cast<std::chrono::milliseconds>(dur));
}

}  // namespace dcb
