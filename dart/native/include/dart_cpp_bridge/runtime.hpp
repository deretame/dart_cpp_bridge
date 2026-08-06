#pragma once

#include "dart_cpp_bridge/channel.hpp"

#include <stdexec/execution.hpp>

#include <exec/asio/asio_config.hpp>
#include <exec/asio/asio_thread_pool.hpp>
#include <exec/asio/use_sender.hpp>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

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

namespace dcb {

// Marker type used to bridge void values through senders that require a
// movable payload (e.g. the co::oneshot channel). `co_await
// dcb::spawn_blocking(f)` for a void `f` completes with dcb::Unit.
struct Unit {};

// P2300 (std::execution / stdexec) scheduler adapter that runs sender work on
// an asio::io_context event loop — the single-threaded io thread. Design
// reference: docs/cpp26_executor_model_usage.md §12.6 (Asio 适配).
//
// `stdexec::schedule(*sched)` returns the official exec::asio adapter sender
// for `asio::post(io_context, use_sender)`: the posted handler runs on the io
// thread, so every step of a chain launched with
// `stdexec::starts_on(*sched, ...)` executes on the io thread, and its
// completion fires back on the io thread. Error/stopped mapping follows the
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
// member; stop() joins the io thread before destruction.
//
// The scheduler is trivially copyable-equivalent in the sense required by the
// stdexec scheduler concept: copies share the underlying io_context pointer.
// current_thread_is_io() delegates to asio's own
// executor_type::running_in_this_thread(), so no thread-id bookkeeping is
// needed (it backs dcb::sync_wait's self-deadlock guard).
class IoContextScheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;

  explicit IoContextScheduler(asio::io_context& ioc) : ioc_(&ioc) {}

  stdexec::sender auto schedule() const noexcept {
    return exec::asio::asio_impl::post(*ioc_, exec::asio::use_sender);
  }

  asio::io_context& io() const noexcept { return *ioc_; }

  asio::io_context::executor_type executor() const noexcept {
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
  asio::io_context* ioc_;
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

  /// Set thread pool size (must be called before start; default 4).
  void set_pool_threads(std::uint32_t n) { pool_threads_ = n ? n : 1; }

  void ensure_running() {
    if (!running()) {
      throw std::runtime_error("runtime stopped");
    }
  }

  asio::io_context& io() { return io_; }

  /// The blocking thread pool executor (asio interop: asio::post(rt.pool(), ...)).
  asio::thread_pool::executor_type pool() { return pool_->get_executor(); }

  /// The scheduler that runs on the single-threaded io_context event loop.
  /// Business senders are launched here (stdexec::starts_on(*io_scheduler(),
  /// sndr)); completions are delivered here.
  IoContextScheduler* io_scheduler() { return &io_sched_; }

  /// The scheduler backed by the blocking thread pool (official
  /// exec::asio::asio_thread_pool adapter; spawn_blocking work).
  auto blocking_scheduler() { return pool_->get_scheduler(); }

  void set_dart_post(DartPostFn fn, void* userdata) {
    post_fn_ = fn;
    post_userdata_ = userdata;
  }

  void post_to_dart(std::int64_t port, const std::uint8_t* data, std::size_t len) {
    if (!running()) {
      return;
    }
    auto fn = post_fn_;
    if (fn) {
      fn(port, data, len, post_userdata_);
    }
  }

 private:
  Runtime();
  ~Runtime();

  asio::io_context io_;
  IoContextScheduler io_sched_{io_};
  std::unique_ptr<exec::asio::asio_thread_pool> pool_;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
  std::unique_ptr<std::thread> io_thread_;
  std::atomic<bool> started_{false};
  std::uint32_t pool_threads_{4};
  DartPostFn post_fn_{nullptr};
  void* post_userdata_{nullptr};
  std::mutex start_stop_mu_;
};

// ---------------------------------------------------------------------------
// Scheduler plumbing (std::exec style)
// ---------------------------------------------------------------------------

namespace detail {

// Environment that exposes a scheduler as both get_scheduler and
// get_start_scheduler. Senders that depend on a scheduler (e.g. exec::task)
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

// Sleep for `dur` on the runtime's io scheduler (official exec::asio adapter:
// asio::steady_timer + async_wait(use_sender); the io thread stays
// responsive). Completion: set_value_t() on the io thread. Supports
// cancellation through the standard stop_token machinery (write_env with an
// inplace_stop_token; a stop request cancels the timer and completes with
// set_stopped).
template <typename Rep, typename Period>
stdexec::sender auto sleep(std::chrono::duration<Rep, Period> dur) {
  auto& rt = Runtime::instance();
  // The timer must outlive the pending async operation: the shared_ptr is
  // held by the then-step until the pipeline completes (or is cancelled).
  auto timer = std::make_shared<asio::steady_timer>(rt.io());
  timer->expires_after(dur);
  return timer->async_wait(exec::asio::use_sender)
       | stdexec::then([timer] { (void)timer; });
}

}  // namespace dcb
