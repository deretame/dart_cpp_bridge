#pragma once

#include "dart_cpp_bridge/asio_executor.hpp"
#include "dart_cpp_bridge/channel.hpp"

#include <stdexec/execution.hpp>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace dcb {

// Marker type used to bridge void values through co::oneshot (which requires
// a movable payload type). `co_await dcb::spawn_blocking(f)` for a void `f`
// yields std::optional<Unit>.
struct Unit {};

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
  asio::thread_pool& pool() { return *pool_; }
  AsioScheduler* scheduler() { return scheduler_.get(); }

  // Launch a sender chain on the io scheduler from a NON-coroutine context.
  //
  // `factory` must be callable with no arguments and return a sender. It is
  // invoked on the io thread; the sender runs on the io scheduler (its
  // completion is delivered back to the io thread via continues_on). Errors
  // are logged and swallowed (fire-and-forget semantics).
  //
  // IMPORTANT: keep `factory` alive until the chain completes. Coroutine
  // lambdas may reference captures from the lambda object; destroying it
  // after start races.
  template <class SenderFactory>
  void spawn_on_asio(SenderFactory&& factory) {
    ensure_running();
    auto* sched = scheduler_.get();
    if (!sched) {
      throw std::runtime_error("runtime scheduler missing");
    }
    asio::post(io_, [factory = std::forward<SenderFactory>(factory), sched]() mutable {
      auto holder = std::make_shared<std::decay_t<decltype(factory)>>(std::move(factory));
      try {
        spawn_on_scheduler((*holder)(), sched);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "[dcb] spawn_on_asio factory error: %s\n", e.what());
      } catch (...) {
        std::fprintf(stderr, "[dcb] spawn_on_asio factory error: unknown\n");
      }
    });
  }

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
  std::unique_ptr<AsioScheduler> scheduler_;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
  std::unique_ptr<std::thread> io_thread_;
  std::unique_ptr<asio::thread_pool> pool_;
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

// Environment that exposes the io scheduler as both get_scheduler and
// get_start_scheduler. Senders that depend on a scheduler (e.g. exec::task)
// need it to compute completion signatures and to run.
struct io_env {
  const AsioScheduler* sched;

  constexpr auto query(stdexec::get_scheduler_t) const noexcept -> const AsioScheduler& {
    return *sched;
  }
  constexpr auto query(stdexec::get_start_scheduler_t) const noexcept -> const AsioScheduler& {
    return *sched;
  }
};

// Shared control block holding the current opstate address. Inner receivers
// hold this instead of a raw pointer: opstates may be moved by the connect()
// return chain (guaranteed elision is not reliable through stdexec's declfn
// wrappers), and the move constructor refreshes the address.
template <typename Op>
struct on_io_ctl {
  Op* op{nullptr};
};

// Inner receiver of on_io_opstate: forwards the completion back to the io
// thread (asio::post) before invoking the outer receiver. The opstate must
// outlive the completion (P2300 guarantee), so capturing it in the posted
// lambda is safe. Its environment exposes the io scheduler so that child
// senders that need a scheduler (e.g. exec::task) can run.
template <typename Op>
struct on_io_inner_receiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<on_io_ctl<Op>> ctl_;

  io_env get_env() const noexcept { return io_env{ctl_->op->sched_}; }

  template <class... As>
  void set_value(As&&... as) && noexcept {
    ctl_->op->post_value(std::make_tuple(std::forward<As>(as)...));
  }

  void set_error(std::exception_ptr ep) && noexcept {
    ctl_->op->post_error(std::move(ep));
  }

  void set_stopped() && noexcept {
    ctl_->op->post_stopped();
  }
};

// Operation state of on_io_sender: connects the child sender, then migrates
// its completion to the io thread.
template <stdexec::sender S, stdexec::receiver Rcvr>
struct on_io_opstate {
  using operation_state_concept = stdexec::operation_state_tag;

  using inner_rcvr_t = on_io_inner_receiver<on_io_opstate>;
  using inner_op_t = stdexec::connect_result_t<S, inner_rcvr_t>;

  const AsioScheduler* sched_;
  Rcvr rcvr_;
  std::shared_ptr<on_io_ctl<on_io_opstate>> ctl_;
  inner_op_t inner_;

  on_io_opstate(const AsioScheduler* sched, S sndr, Rcvr rcvr)
    : sched_(sched),
      rcvr_(std::move(rcvr)),
      ctl_(std::make_shared<on_io_ctl<on_io_opstate>>()),
      inner_(stdexec::connect(std::move(sndr), inner_rcvr_t{ctl_})) {
    ctl_->op = this;
  }

  on_io_opstate(on_io_opstate&& o) noexcept
    : sched_(o.sched_),
      rcvr_(std::move(o.rcvr_)),
      ctl_(std::move(o.ctl_)),
      inner_(std::move(o.inner_)) {
    // The inner receivers hold ctl_, so refreshing the address here keeps
    // them pointing at this (moved-to) opstate.
    ctl_->op = this;
  }

  on_io_opstate(const on_io_opstate&) = delete;
  on_io_opstate& operator=(const on_io_opstate&) = delete;
  on_io_opstate& operator=(on_io_opstate&&) = delete;

  void start() noexcept { stdexec::start(inner_); }

  template <typename Tuple>
  void post_value(Tuple&& vals) {
    try {
      asio::post(sched_->io(),
                 [this, vals = std::forward<Tuple>(vals)]() mutable {
                   std::apply(
                       [this](auto&&... a) {
                         stdexec::set_value(std::move(rcvr_),
                                            std::forward<decltype(a)>(a)...);
                       },
                       std::move(vals));
                 });
    } catch (...) {
      // asio::post only throws on allocation failure.
      stdexec::set_error(std::move(rcvr_),
                         std::make_exception_ptr(std::bad_alloc()));
    }
  }

  void post_error(std::exception_ptr ep) {
    try {
      asio::post(sched_->io(), [this, ep]() mutable {
        stdexec::set_error(std::move(rcvr_), ep);
      });
    } catch (...) {
      stdexec::set_error(std::move(rcvr_),
                         std::make_exception_ptr(std::bad_alloc()));
    }
  }

  void post_stopped() {
    try {
      asio::post(sched_->io(), [this]() mutable {
        stdexec::set_stopped(std::move(rcvr_));
      });
    } catch (...) {
      stdexec::set_error(std::move(rcvr_),
                         std::make_exception_ptr(std::bad_alloc()));
    }
  }
};

// Sender wrapper that migrates the child's completion to the io thread.
template <stdexec::sender S>
struct on_io_sender {
  using sender_concept = stdexec::sender_tag;

  // Attributes exposed via get_env(): algorithms (then/let_value, ...) probe
  // the completion behavior of their child sender.
  struct attrs {
    constexpr auto query(
        stdexec::__get_completion_behavior_t<stdexec::set_value_t>) const noexcept {
      return stdexec::__completion_behavior::__inline_completion;
    }
    constexpr auto operator==(const attrs&) const noexcept -> bool = default;
  };

  static constexpr auto get_env() noexcept -> attrs { return {}; }

  S sndr_;
  const AsioScheduler* sched_;

  template <stdexec::receiver Rcvr>
  on_io_opstate<S, Rcvr> connect(Rcvr rcvr) && {
    return on_io_opstate<S, Rcvr>(sched_, std::move(sndr_), std::move(rcvr));
  }

  // Forward the child's completion signatures. The child is queried with the
  // io scheduler injected into the environment: senders that depend on a
  // scheduler (e.g. exec::task) need it to compute their signatures.
  friend auto tag_invoke(stdexec::get_completion_signatures_t,
                         const on_io_sender& self, auto env)
      -> decltype(stdexec::get_completion_signatures(
          self.sndr_, io_env{self.sched_})) {
    return stdexec::get_completion_signatures(self.sndr_, io_env{self.sched_});
  }
};

}  // namespace detail

// Move the completion of `sndr` onto the runtime's io thread. The sender may
// complete on any thread; every downstream step then runs on the io thread.
// Requires the runtime to be started.
template <stdexec::sender S>
auto on_io(S&& sndr) {
  auto& rt = Runtime::instance();
  auto* sched = rt.scheduler();
  if (!sched) {
    throw std::runtime_error("runtime scheduler missing");
  }
  return detail::on_io_sender<std::decay_t<S>>{std::forward<S>(sndr), sched};
}

// Receiver used by spawn_on_scheduler: swallows every completion signal
// (errors are logged to stderr). A custom receiver is used instead of
// start_detached because continues_on always adds a
// set_error_t(std::exception_ptr) completion (allocation failures), which
// start_detached rejects at compile time.
struct fire_and_forget_receiver {
  using receiver_concept = stdexec::receiver_tag;

  template <class... Vs>
  void set_value(Vs&&...) && noexcept {
  }

  void set_error(std::exception_ptr ep) && noexcept {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[dcb] spawned sender error: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "[dcb] spawned sender error: unknown\n");
    }
  }

  void set_stopped() && noexcept {
  }
};

// Fire-and-forget opstate holder: heap-allocated so the opstate outlives
// start() until completion (P2300 requirement — an opstate must not be
// destroyed before the completion signal fires). Released on completion.
//
// The opstate member is constructed in-place (no moves): stdexec's sexpr
// opstates are immovable, so any make_unique/emplace forwarding that forces a
// move would fail to compile.
template <stdexec::sender S, typename Rcvr>
struct faf_state {
  // Use the exact connect return type (connect_result_t may decay cv/refs).
  struct rcvr_t : Rcvr {
    std::shared_ptr<faf_state>& self_ref;

    rcvr_t(Rcvr rcvr, std::shared_ptr<faf_state>& sref)
      : Rcvr(std::move(rcvr)), self_ref(sref) {}

    template <class... As>
    void set_value(As&&... as) && noexcept {
      static_cast<Rcvr&&>(*this).set_value(std::forward<As>(as)...);
      self_ref.reset();
    }

    void set_error(std::exception_ptr ep) && noexcept {
      static_cast<Rcvr&&>(*this).set_error(ep);
      self_ref.reset();
    }

    void set_stopped() && noexcept {
      static_cast<Rcvr&&>(*this).set_stopped();
      self_ref.reset();
    }
  };

  using op_t = decltype(stdexec::connect(std::declval<S>(),
                                         std::declval<rcvr_t>()));

  op_t op;
  std::shared_ptr<faf_state> self;

  faf_state(S sndr, Rcvr rcvr)
    : op(stdexec::connect(std::move(sndr), rcvr_t{std::move(rcvr), self})) {}
};

// Start `sndr` detached, keeping the opstate alive until it completes.
// `rcvr` receives the completion signals (on whatever thread the sender
// completes). Errors are delivered to the receiver; nothing is rethrown.
template <stdexec::sender S, stdexec::receiver Rcvr>
void start_detached(S&& sndr, Rcvr rcvr) {
  using state_t = faf_state<std::decay_t<S>, Rcvr>;
  auto state = std::make_shared<state_t>(std::forward<S>(sndr), std::move(rcvr));
  state->self = state;
  stdexec::start(state->op);
}

// Start a sender on the io scheduler, swallowing errors (fire-and-forget).
// Errors are logged to stderr. Sends the completion back to the io thread.
template <stdexec::sender S>
void spawn_on_scheduler(S&& sndr, AsioScheduler* sched) {
  auto chain = detail::on_io_sender<std::decay_t<S>>{std::forward<S>(sndr), sched};
  start_detached(std::move(chain), fire_and_forget_receiver{});
}

// Block the calling thread until `sndr` completes. Returns the value or
// rethrows the sender's error (mirrors async_simple's syncAwait).
//
//   int v = *dcb::sync_wait(dcb::spawn(add(a, b)));  // -> optional<int>
//
// NEVER call on the io thread: blocking the io thread while the awaited
// sender needs it is a self-deadlock. AsioScheduler::current_thread_is_io()
// identifies the io thread and this wrapper rejects it with std::logic_error.
template <stdexec::sender S>
auto sync_wait(S&& sndr) {
  auto* sched = Runtime::instance().scheduler();
  if (sched && sched->current_thread_is_io()) {
    throw std::logic_error(
      "dcb::sync_wait must not be called on the io thread (self-deadlock)");
  }
  return stdexec::sync_wait(std::forward<S>(sndr));
}

// Launch a sender on the runtime's io scheduler from a NON-coroutine context
// and bind its completion back to the io thread. Returns the sender chain
// (NOT yet started) — the caller decides how to trigger it:
//
//   // 1) Block a normal (non-io) thread until the result is ready:
//   int v = *dcb::sync_wait(dcb::spawn(sndr));
//
//   // 2) Fire-and-forget (errors are logged and swallowed):
//   dcb::spawn_detached(std::move(sndr));
//
// If you are already INSIDE a coroutine running on io, do not use spawn — just
// `co_await dcb::on_io(std::move(sndr))` (or co_await the sender directly if
// its completion thread is acceptable).
template <stdexec::sender S>
auto spawn(S&& sndr) {
  return on_io(std::forward<S>(sndr));
}

// Fire-and-forget: launch a sender on the io scheduler and discard its result
// (both value and exception are swallowed; exceptions are logged).
template <stdexec::sender S>
void spawn_detached(S&& sndr) {
  auto& rt = Runtime::instance();
  rt.ensure_running();
  auto* sched = rt.scheduler();
  if (!sched) {
    throw std::runtime_error("runtime scheduler missing");
  }
  spawn_on_scheduler(std::forward<S>(sndr), sched);
}

// Run a blocking callable on the runtime's thread pool and return a sender
// that resolves to its result. The io thread is never blocked: the callable
// runs on a pool thread while the awaiting coroutine suspends. Completion
// signatures: set_value_t(std::optional<T>) / set_error_t(std::exception_ptr),
// delivered on the io thread (via on_io). Void callables are bridged through
// dcb::Unit (optional<Unit> completes on success).
//
//   // inside a coroutine running on io:
//   auto v = co_await dcb::spawn_blocking([&] { return heavyComputation(); });
//
//   // block a normal (non-io) thread for the result:
//   auto v = *dcb::sync_wait(dcb::spawn(dcb::spawn_blocking(f)));
//
// Exceptions thrown by the callable are captured on the pool thread and
// rethrown at the awaiter (set_error).
template <class F>
auto spawn_blocking(F&& f) -> stdexec::sender auto {
  using T = std::invoke_result_t<std::decay_t<F>>;
  using WireT = std::conditional_t<std::is_void_v<T>, Unit, T>;
  auto& rt = Runtime::instance();
  rt.ensure_running();
  auto [tx, rx] = co::oneshot::channel<WireT>();
  asio::post(rt.pool(), [f = std::forward<F>(f), tx = std::move(tx)]() mutable {
    try {
      if constexpr (std::is_void_v<T>) {
        f();
        tx.send(Unit{});
      } else {
        tx.send(f());
      }
    } catch (...) {
      tx.send_error(std::current_exception());
    }
  });
  return on_io(std::move(rx));
}

// Sleep for `dur` on the runtime's io scheduler (timer-based, io thread stays
// responsive). Completion: set_value_t() on the io thread.
template <typename Rep, typename Period>
stdexec::sender auto sleep(std::chrono::duration<Rep, Period> dur) {
  auto& rt = Runtime::instance();
  auto* sched = rt.scheduler();
  if (!sched) {
    throw std::runtime_error("runtime scheduler missing");
  }
  return sched->schedule_at(dur);
}

}  // namespace dcb
