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
#include <mutex>
#include <stdexec/execution.hpp>
#include <thread>
#include <type_traits>
#include <utility>

namespace dcb {

// Marker type used to bridge void values through senders that require a
// movable payload (e.g. the co::oneshot channel). `co_await
// dcb::spawn_blocking(f)` for a void `f` completes with dcb::Unit.
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

  /// The scheduler that runs on the single-threaded io_context event loop.
  /// Business senders are launched here; completions are delivered here.
  IoContextScheduler* io_scheduler() { return io_sched_.get(); }
  /// The scheduler backed by the blocking thread pool (spawn_blocking work).
  PoolScheduler* blocking_scheduler() { return pool_sched_.get(); }

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
  std::unique_ptr<IoContextScheduler> io_sched_;
  std::unique_ptr<PoolScheduler> pool_sched_;
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

// Shared control block holding the current opstate address. Inner receivers
// hold this instead of a raw pointer: opstates may be moved by the connect()
// return chain (guaranteed elision is not reliable through stdexec's declfn
// wrappers), and the move constructor refreshes the address.
template <typename Op>
struct op_ctl {
  Op* op{nullptr};
};

// Inner receiver of on_scheduler_opstate: forwards the completion back onto
// the target scheduler (post) before invoking the outer receiver. The opstate
// must outlive the completion (P2300 guarantee), so capturing it in the
// posted lambda is safe. Its environment exposes the scheduler so that child
// senders that need one (e.g. exec::task) can run.
template <typename Sched, typename Op>
struct on_scheduler_inner_receiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<op_ctl<Op>> ctl_;

  sched_env<Sched> get_env() const noexcept {
    return sched_env<Sched>{ctl_->op->sched_};
  }

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

// Operation state of on_scheduler_sender: connects the child sender, starts
// it ON the target scheduler (starts-on semantics), then migrates its
// completion back to that same scheduler.
template <stdexec::sender S, typename Sched, stdexec::receiver Rcvr>
struct on_scheduler_opstate {
  using operation_state_concept = stdexec::operation_state_tag;

  using inner_rcvr_t = on_scheduler_inner_receiver<Sched, on_scheduler_opstate>;
  using inner_op_t = stdexec::connect_result_t<S, inner_rcvr_t>;

  const Sched* sched_;
  Rcvr rcvr_;
  std::shared_ptr<op_ctl<on_scheduler_opstate>> ctl_;
  inner_op_t inner_;

  on_scheduler_opstate(const Sched* sched, S sndr, Rcvr rcvr)
    : sched_(sched),
      rcvr_(std::move(rcvr)),
      ctl_(std::make_shared<op_ctl<on_scheduler_opstate>>()),
      inner_(stdexec::connect(std::move(sndr), inner_rcvr_t{ctl_})) {
    ctl_->op = this;
  }

  on_scheduler_opstate(on_scheduler_opstate&& o) noexcept
    : sched_(o.sched_),
      rcvr_(std::move(o.rcvr_)),
      ctl_(std::move(o.ctl_)),
      inner_(std::move(o.inner_)) {
    // The inner receivers hold ctl_, so refreshing the address here keeps
    // them pointing at this (moved-to) opstate.
    ctl_->op = this;
  }

  on_scheduler_opstate(const on_scheduler_opstate&) = delete;
  on_scheduler_opstate& operator=(const on_scheduler_opstate&) = delete;
  on_scheduler_opstate& operator=(on_scheduler_opstate&&) = delete;

  // starts-on: the child chain begins running on the scheduler's thread(s),
  // so business code (senders, then callbacks) executes there.
  void start() noexcept {
    try {
      asio::post(sched_->executor(), [op = this]() { stdexec::start(op->inner_); });
    } catch (...) {
      stdexec::set_error(std::move(rcvr_),
                         std::make_exception_ptr(std::bad_alloc()));
    }
  }

  template <typename Tuple>
  void post_value(Tuple&& vals) {
    try {
      asio::post(sched_->executor(),
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
      asio::post(sched_->executor(), [this, ep]() mutable {
        stdexec::set_error(std::move(rcvr_), ep);
      });
    } catch (...) {
      stdexec::set_error(std::move(rcvr_),
                         std::make_exception_ptr(std::bad_alloc()));
    }
  }

  void post_stopped() {
    try {
      asio::post(sched_->executor(), [this]() mutable {
        stdexec::set_stopped(std::move(rcvr_));
      });
    } catch (...) {
      stdexec::set_error(std::move(rcvr_),
                         std::make_exception_ptr(std::bad_alloc()));
    }
  }
};

// Sender wrapper that starts the child on `sched` and migrates its
// completion back to `sched`.
template <stdexec::sender S, typename Sched>
struct on_scheduler_sender {
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
  const Sched* sched_;

  template <stdexec::receiver Rcvr>
  on_scheduler_opstate<S, Sched, Rcvr> connect(Rcvr rcvr) && {
    return on_scheduler_opstate<S, Sched, Rcvr>(sched_, std::move(sndr_),
                                                std::move(rcvr));
  }

  // Forward the child's completion signatures. The child is queried with the
  // scheduler injected into the environment: senders that depend on a
  // scheduler (e.g. exec::task) need it to compute their signatures.
  friend auto tag_invoke(stdexec::get_completion_signatures_t,
                         const on_scheduler_sender& self, auto env)
      -> decltype(stdexec::get_completion_signatures(
          self.sndr_, sched_env<Sched>{self.sched_})) {
    return stdexec::get_completion_signatures(self.sndr_,
                                              sched_env<Sched>{self.sched_});
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

// Receiver used for fire-and-forget launches: swallows every completion
// signal (errors are logged to stderr).
struct fire_and_forget_receiver {
  using receiver_concept = stdexec::receiver_tag;

  template <class... Vs>
  void set_value(Vs&&...) && noexcept {
  }

  void set_error(std::exception_ptr ep) && noexcept {
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[dcb] sender error: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "[dcb] sender error: unknown\n");
    }
  }

  void set_stopped() && noexcept {
  }
};

}  // namespace detail

// Start `sndr` detached, keeping the opstate alive until it completes.
// `rcvr` receives the completion signals (on whatever thread the sender
// completes). Errors are delivered to the receiver; nothing is rethrown.
template <stdexec::sender S, stdexec::receiver Rcvr>
void start_detached(S&& sndr, Rcvr rcvr) {
  using state_t = detail::faf_state<std::decay_t<S>, Rcvr>;
  auto state = std::make_shared<state_t>(std::forward<S>(sndr), std::move(rcvr));
  state->self = state;
  stdexec::start(state->op);
}

// Launch a sender chain on `sched`: the child starts on the scheduler's
// thread(s) and its completion is delivered back there. Fire-and-forget
// (errors are logged and swallowed).
template <stdexec::sender S, typename Sched>
void start_on_scheduler(S&& sndr, const Sched* sched) {
  auto chain = detail::on_scheduler_sender<std::decay_t<S>, Sched>{
      std::forward<S>(sndr), sched};
  start_detached(std::move(chain), detail::fire_and_forget_receiver{});
}

// Start a sender chain on the runtime's io scheduler (starts-on io; the
// whole chain — connect, then callbacks, completion — runs on the io thread).
// Fire-and-forget: errors are logged and swallowed.
template <stdexec::sender S>
void start_on_io(S&& sndr) {
  auto& rt = Runtime::instance();
  rt.ensure_running();
  auto* sched = rt.io_scheduler();
  if (!sched) {
    throw std::runtime_error("runtime scheduler missing");
  }
  start_on_scheduler(std::forward<S>(sndr), sched);
}

// Move the completion of `sndr` onto `sched`: the child starts on the
// scheduler's thread(s) and every downstream step then runs there too.
// Requires the runtime to be started (for on_io).
template <stdexec::sender S, typename Sched>
auto on_scheduler(S&& sndr, const Sched* sched) {
  return detail::on_scheduler_sender<std::decay_t<S>, Sched>{
      std::forward<S>(sndr), sched};
}

// Run a sender chain on the runtime's io scheduler (starts-on io).
template <stdexec::sender S>
auto on_io(S&& sndr) {
  auto& rt = Runtime::instance();
  auto* sched = rt.io_scheduler();
  if (!sched) {
    throw std::runtime_error("runtime scheduler missing");
  }
  return on_scheduler(std::forward<S>(sndr), sched);
}

// Block the calling thread until `sndr` completes. Returns the value or
// rethrows the sender's error (mirrors std::exec's sync_wait semantics, with
// a runtime deadlock guard):
//
//   int v = std::get<0>(*dcb::sync_wait(dcb::on_io(add(a, b))));
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

// Run a blocking callable on the runtime's thread pool and return a sender
// that resolves to its result. The io thread is never blocked: the callable
// runs on a pool thread while the awaiting coroutine suspends. Completion
// signatures: set_value_t(WireT) (WireT = T, or dcb::Unit for void) /
// set_error_t(std::exception_ptr), delivered on the io thread.
//
//   // inside a coroutine running on io:
//   auto v = co_await dcb::spawn_blocking([&] { return heavyComputation(); });
//
//   // block a normal (non-io) thread for the result:
//   auto v = std::get<0>(*dcb::sync_wait(dcb::on_io(dcb::spawn_blocking(f))));
//
// Exceptions thrown by the callable are captured on the pool thread and
// rethrown at the awaiter (set_error).
template <class F>
auto spawn_blocking(F&& f) -> stdexec::sender auto {
  using T = std::invoke_result_t<std::decay_t<F>>;
  using WireT = std::conditional_t<std::is_void_v<T>, Unit, T>;
  auto& rt = Runtime::instance();
  rt.ensure_running();
  auto* pool_sched = rt.blocking_scheduler();
  if (!pool_sched) {
    throw std::runtime_error("runtime scheduler missing");
  }
  // std::exec style: schedule the callable onto the blocking scheduler, then
  // migrate the completion back to the io scheduler. Exceptions inside the
  // callable become set_error automatically.
  auto work = stdexec::just() | stdexec::then([f = std::forward<F>(f)]() -> WireT {
    if constexpr (std::is_void_v<T>) {
      f();
      return Unit{};
    } else {
      return f();
    }
  });
  auto pool_side = detail::on_scheduler_sender<std::decay_t<decltype(work)>,
                                               PoolScheduler>{
      std::move(work), pool_sched};
  return on_io(std::move(pool_side));
}

// Sleep for `dur` on the runtime's io scheduler (timer-based, io thread stays
// responsive). Completion: set_value_t() on the io thread. Supports
// cancellation through the standard stop_token machinery (write_env with an
// inplace_stop_token; a stop request completes with set_stopped).
template <typename Rep, typename Period>
stdexec::sender auto sleep(std::chrono::duration<Rep, Period> dur) {
  auto& rt = Runtime::instance();
  auto* sched = rt.io_scheduler();
  if (!sched) {
    throw std::runtime_error("runtime scheduler missing");
  }
  return sched->schedule_at(dur);
}

}  // namespace dcb
