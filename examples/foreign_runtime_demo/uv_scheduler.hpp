#pragma once

// uv_scheduler.hpp — libuv event loop wrapped as a stdexec scheduler, plus
// senders for common loop operations.
//
// This is the stdexec-era replacement for the old ForeignExecutor +
// foreign_runtime.h C API (see docs/cpp26_executor_model_usage.md §12.6 for
// the Asio equivalent). A foreign event loop only needs to provide a
// scheduler; bridge business code then composes it with the standard sender
// algorithms:
//
//   // run work on the uv loop thread (completion stays there):
//   auto s = stdexec::starts_on(uv_sched, stdexec::just() | stdexec::then(f));
//
//   // delay on the uv loop (cancellable via the receiver's stop token):
//   auto s = uv_sched.schedule_after(200ms);
//
//   // CPU-bound work via uv_queue_work (thread pool + loop-thread completion):
//   auto s = uv_sched.uv_work([] { return heavy(); });
//
// Threading contract (libuv): uv handles are only ever touched on the loop
// thread. Cross-thread traffic goes through a start queue (mutex + uv_async
// wake): opstate start() enqueues a task, the loop thread drains it and runs
// the task. Cancellation is arbitrated by atomic claims so that exactly one
// of {completion, cancellation} wins and the loser never touches the other
// party's state.
//
// Lifetime: the scheduler must not outlive the uv_loop_t it wraps (the owner
// UvWorker guarantees the order: loop_ member + stop() joins the thread).

#include <uv.h>

#include <stdexec/execution.hpp>
#include <stdexec/stop_token.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace foreign_demo {

// ---------------------------------------------------------------------------
// Shared state: the loop, the wake-up async handle and the start queue.
// Owned by UvWorker; the scheduler holds a shared_ptr so copies stay cheap.
// ---------------------------------------------------------------------------
struct UvSchedState {
  uv_loop_t* loop{nullptr};
  uv_async_t async{};
  std::mutex mu;
  struct Node {
    Node* next{nullptr};
  };
  Node* head{nullptr};
  Node* tail{nullptr};
  bool closed{false};  // set under mu by the owner before loop teardown
  std::atomic<bool> async_ready{false};

  void push(Node* n) {
    if (tail) {
      tail->next = n;
    } else {
      head = n;
    }
    tail = n;
    n->next = nullptr;
  }

  Node* pop_all() {
    Node* h = head;
    head = tail = nullptr;
    return h;
  }

  // Wake the loop so it drains the start queue. Safe from any thread.
  void wake() {
    if (async_ready.load(std::memory_order_acquire)) {
      uv_async_send(&async);
    }
  }

  // Base of every queued start task. run() executes on the loop thread and
  // returns false when the operation was cancelled before it could start.
  struct StartNode : Node {
    virtual bool run() = 0;
    // Heap-allocated nodes (TimerInitNode / TimerCancelNode / WorkStartNode)
    // free themselves after run(); the embedded schedule opstate overrides
    // this to a no-op.
    virtual void dispose() { delete this; }
  };

  // Run on the loop thread; drains the start queue and runs each node.
  static void on_async(uv_async_t* h) {
    auto* st = static_cast<UvSchedState*>(h->data);
    Node* node;
    {
      std::lock_guard lock(st->mu);
      node = st->pop_all();
    }
    while (node) {
      Node* next = node->next;
      node->next = nullptr;
      auto* start = static_cast<StartNode*>(node);
      start->run();
      start->dispose();  // heap nodes free themselves; embedded opstate no-ops
      node = next;
    }
  }
};

// ---------------------------------------------------------------------------
// UvScheduler: schedule() -> "run once on the loop thread" sender.
// ---------------------------------------------------------------------------
class UvScheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;

  // Default-constructed scheduler is empty; assign from UvWorker::scheduler()
  // before use (e.g. `UvScheduler s; { lock; s = worker.scheduler(); }`).
  explicit UvScheduler(std::shared_ptr<UvSchedState> st = {}) : st_(std::move(st)) {}

  stdexec::sender auto schedule() const noexcept {
    return schedule_sender{st_};
  }

  // Timer: completes with set_value() on the loop thread after `d` elapses.
  // Cancellable: a stop request wins the claim and completes with
  // set_stopped(); destroying the operation state cancels silently.
  stdexec::sender auto schedule_after(std::chrono::milliseconds d) const noexcept {
    return timer_sender{st_, d};
  }

  // CPU-bound work via uv_queue_work: `f` runs on libuv's thread pool, the
  // completion (value or exception) is delivered on the loop thread.
  template <class F>
  stdexec::sender auto uv_work(F f) const noexcept {
    return work_sender<F>{st_, std::move(f)};
  }

  bool operator==(const UvScheduler&) const noexcept = default;

 private:
  std::shared_ptr<UvSchedState> st_;

  // ─── schedule sender ────────────────────────────────────────────────────
  struct schedule_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;
    std::shared_ptr<UvSchedState> st_;

    template <class Rcvr>
    struct opstate : UvSchedState::StartNode {
      using operation_state_concept = stdexec::operation_state_tag;

      enum Claim : int { kQueued = 0, kRunning = 1, kCancelled = 2 };

      std::shared_ptr<UvSchedState> st_;
      Rcvr rcvr_;
      std::atomic<int> claim_{kQueued};

      opstate(std::shared_ptr<UvSchedState> st, Rcvr rcvr)
        : st_(std::move(st)), rcvr_(std::move(rcvr)) {}

      opstate(opstate&&) = delete;  // P2300: operation states are immovable
      opstate(const opstate&) = delete;
      opstate& operator=(const opstate&) = delete;

      ~opstate() {
        std::lock_guard lock(st_->mu);
        int expected = kQueued;
        if (claim_.compare_exchange_strong(expected, kCancelled)) {
          // Still queued: unlink so the loop never runs a dead opstate.
          Node** pp = &st_->head;
          while (*pp && *pp != this) {
            pp = &(*pp)->next;
          }
          if (*pp == this) {
            *pp = next;
            if (st_->tail == this) {
              st_->tail = nullptr;
            }
          }
        }
      }

      void start() & noexcept {
        bool closed = false;
        {
          std::lock_guard lock(st_->mu);
          if (st_->closed) {
            closed = true;
          } else {
            st_->push(this);
          }
        }
        if (closed) {
          int expected = kQueued;
          if (claim_.compare_exchange_strong(expected, kCancelled)) {
            stdexec::set_stopped(std::move(rcvr_));
          }
          return;
        }
        st_->wake();
      }

      bool run() override {  // loop thread
        // Claim the execution right: the destructor may have cancelled us
        // while we were still queued (it unlinks and claims kCancelled).
        int expected = kQueued;
        if (!claim_.compare_exchange_strong(expected, kRunning)) {
          return false;  // cancelled; the destructor owns the receiver
        }
        stdexec::set_value(std::move(rcvr_));
        return true;
      }

      void dispose() override {}  // embedded in the parent opstate, not leaked
    };

    template <class Rcvr>
    opstate<Rcvr> connect(Rcvr rcvr) && {
      return opstate<Rcvr>{st_, std::move(rcvr)};
    }
  };

  // ─── timer sender ───────────────────────────────────────────────────────
  struct timer_sender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(), stdexec::set_stopped_t()>;
    std::shared_ptr<UvSchedState> st_;
    std::chrono::milliseconds delay_;

    template <class Rcvr>
    struct opstate;
    template <class Rcvr>
    opstate<Rcvr> connect(Rcvr rcvr) &&;
  };

  // ─── uv_work sender ─────────────────────────────────────────────────────
  template <class F>
  struct work_sender {
    using sender_concept = stdexec::sender_tag;
    using result_t = std::invoke_result_t<F>;
    using value_sig = std::conditional_t<std::is_void_v<result_t>,
                                         stdexec::set_value_t(),
                                         stdexec::set_value_t(result_t)>;
    using completion_signatures =
      stdexec::completion_signatures<value_sig, stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;
    std::shared_ptr<UvSchedState> st_;
    F fn_;

    template <class Rcvr>
    struct opstate;
    template <class Rcvr>
    opstate<Rcvr> connect(Rcvr rcvr) &&;
  };
};

// ---------------------------------------------------------------------------
// Timer implementation. The receiver and the uv_timer_t live in a shared
// TimerState (self-held until the close callback) so cancellation from any
// thread never races a completion on the loop thread: an atomic claim
// decides the single winner.
// ---------------------------------------------------------------------------
namespace uv_detail {

// Common prefix of every TimerState<Rcvr> (fixed layout so the UvWorker's
// uv_walk cleanup can reach the typed close callback through h->data).
struct TimerStateBase {
  uv_timer_t timer{};
  void (*close_fn)(uv_handle_t*){nullptr};
};

template <class Rcvr>
struct TimerState : TimerStateBase {
  enum Claim : int { kPending = 0, kValue = 1, kStopped = 2, kCancelled = 3 };

  std::atomic<int> claim{kPending};
  std::unique_ptr<Rcvr> rcvr;
  // Self-hold: keeps the state alive from init until the close callback.
  std::shared_ptr<TimerState> self;
  // Exactly one of {init fallback, timer callback, cancel node} closes the
  // handle; the winner also stops the timer.
  std::atomic<bool> closed{false};
  bool inited{false};  // loop thread only

  static void on_timer(uv_timer_t* h) {
    auto* ts = static_cast<TimerState*>(h->data);
    int expected = kPending;
    if (ts->claim.compare_exchange_strong(expected, kValue)) {
      auto rcvr = std::move(ts->rcvr);
      if (!ts->closed.exchange(true)) {
        uv_timer_stop(&ts->timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&ts->timer), &TimerState::on_close);
      }
      stdexec::set_value(std::move(*rcvr));
    } else {
      // Cancelled/stopped already claimed: release the handle only.
      if (!ts->closed.exchange(true)) {
        uv_timer_stop(&ts->timer);
        uv_close(reinterpret_cast<uv_handle_t*>(&ts->timer), &TimerState::on_close);
      }
    }
  }

  static void on_close(uv_handle_t* h) {
    auto* ts = static_cast<TimerState*>(h->data);
    ts->self.reset();  // may destroy `ts`; do not touch it afterwards
  }
};

// Init task: runs on the loop thread; initializes and starts the timer, or
// closes it immediately if a cancel claimed the state first.
template <class Rcvr>
struct TimerInitNode : UvSchedState::StartNode {
  uv_loop_t* loop_;
  std::shared_ptr<TimerState<Rcvr>> ts_;

  TimerInitNode(uv_loop_t* loop, std::shared_ptr<TimerState<Rcvr>> ts)
    : loop_(loop), ts_(std::move(ts)) {}

  bool run() override {
    ts_->self = ts_;  // hold until the close callback
    ts_->close_fn = &TimerState<Rcvr>::on_close;
    uv_timer_init(loop_, &ts_->timer);
    ts_->timer.data = ts_.get();
    ts_->inited = true;
    if (ts_->claim.load(std::memory_order_acquire) != TimerState<Rcvr>::kPending) {
      if (!ts_->closed.exchange(true)) {
        uv_close(reinterpret_cast<uv_handle_t*>(&ts_->timer), &TimerState<Rcvr>::on_close);
      }
      return true;
    }
    uv_timer_start(&ts_->timer, &TimerState<Rcvr>::on_timer, /*timeout*/ 0, /*repeat*/ 0);
    return true;
  }
};

// Cancel/stop-complete task: runs on the loop thread.
template <class Rcvr>
struct TimerCancelNode : UvSchedState::StartNode {
  std::shared_ptr<TimerState<Rcvr>> ts_;
  bool complete_stopped_;

  TimerCancelNode(std::shared_ptr<TimerState<Rcvr>> ts, bool complete_stopped)
    : ts_(std::move(ts)), complete_stopped_(complete_stopped) {}

  bool run() override {
    if (!ts_->inited) {
      // The init node will observe the claim and close without starting.
      // Deliver the stopped completion here so the receiver never hangs.
      if (complete_stopped_ && ts_->rcvr) {
        auto rcvr = std::move(ts_->rcvr);
        stdexec::set_stopped(std::move(*rcvr));
      }
      return true;
    }
    if (!ts_->closed.exchange(true)) {
      uv_timer_stop(&ts_->timer);
      uv_close(reinterpret_cast<uv_handle_t*>(&ts_->timer), &TimerState<Rcvr>::on_close);
    }
    if (complete_stopped_ && ts_->rcvr) {
      auto rcvr = std::move(ts_->rcvr);
      stdexec::set_stopped(std::move(*rcvr));
    }
    return true;
  }
};

}  // namespace uv_detail

template <class Rcvr>
struct UvScheduler::timer_sender::opstate {
  using operation_state_concept = stdexec::operation_state_tag;

  struct StopCb {
    opstate* self;
    void operator()() noexcept { self->on_stop(); }
  };

  std::shared_ptr<UvSchedState> st_;
  Rcvr rcvr_;
  std::chrono::milliseconds delay_;
  std::shared_ptr<uv_detail::TimerState<Rcvr>> ts_;
  std::optional<stdexec::inplace_stop_callback<StopCb>> stop_reg_;

  opstate(std::shared_ptr<UvSchedState> st, Rcvr rcvr, std::chrono::milliseconds d)
    : st_(std::move(st)), rcvr_(std::move(rcvr)), delay_(d) {}

  opstate(opstate&&) = delete;  // P2300: operation states are immovable
  opstate(const opstate&) = delete;
  opstate& operator=(const opstate&) = delete;

  ~opstate() {
    // Destroy the stop registration first: its destructor synchronizes with
    // an in-flight stop callback.
    stop_reg_.reset();
    if (ts_) {
      int expected = uv_detail::TimerState<Rcvr>::kPending;
      if (ts_->claim.compare_exchange_strong(expected,
                                             uv_detail::TimerState<Rcvr>::kCancelled)) {
        enqueue_cancel(/*complete_stopped=*/false);
      }
    }
  }

  void start() & noexcept {
    // Read the environment before moving the receiver into the timer state.
    auto tok = stdexec::get_stop_token(stdexec::get_env(rcvr_));
    auto ts = std::make_shared<uv_detail::TimerState<Rcvr>>();
    ts->rcvr = std::make_unique<Rcvr>(std::move(rcvr_));
    ts_ = std::move(ts);

    if (tok.stop_possible()) {
      stop_reg_.emplace(tok, StopCb{this});
    }

    bool closed = false;
    {
      std::lock_guard lock(st_->mu);
      closed = st_->closed;
      if (!closed) {
        st_->push(new uv_detail::TimerInitNode<Rcvr>(st_->loop, ts_));
      }
    }
    if (closed) {
      int expected = uv_detail::TimerState<Rcvr>::kPending;
      if (ts_->claim.compare_exchange_strong(expected,
                                             uv_detail::TimerState<Rcvr>::kStopped)) {
        auto rcvr = std::move(ts_->rcvr);
        stdexec::set_stopped(std::move(*rcvr));
      }
      return;
    }
    st_->wake();
  }

  void on_stop() noexcept {
    // Runs on the thread that requested the stop.
    int expected = uv_detail::TimerState<Rcvr>::kPending;
    if (ts_->claim.compare_exchange_strong(expected,
                                           uv_detail::TimerState<Rcvr>::kStopped)) {
      enqueue_cancel(/*complete_stopped=*/true);
    }
  }

 private:
  void enqueue_cancel(bool complete_stopped) {
    bool closed = false;
    {
      std::lock_guard lock(st_->mu);
      closed = st_->closed;
      if (!closed) {
        st_->push(new uv_detail::TimerCancelNode<Rcvr>(ts_, complete_stopped));
      }
    }
    if (closed) {
      // Loop is gone: nothing will ever run; complete stopped inline if the
      // receiver still needs it.
      if (complete_stopped && ts_->rcvr) {
        auto rcvr = std::move(ts_->rcvr);
        stdexec::set_stopped(std::move(*rcvr));
      }
      return;
    }
    st_->wake();
  }
};

template <class Rcvr>
UvScheduler::timer_sender::opstate<Rcvr> UvScheduler::timer_sender::connect(Rcvr rcvr) && {
  return opstate<Rcvr>{st_, std::move(rcvr), delay_};
}

// ---------------------------------------------------------------------------
// uv_work implementation: uv_queue_work with a heap-allocated work state.
// The work callback runs on libuv's thread pool; the after-work callback
// (loop thread) completes the receiver. Cancelling (destroying the opstate)
// discards the result when it arrives — uv_queue_work cannot be aborted.
// ---------------------------------------------------------------------------
namespace uv_detail {

template <class F, class Rcvr>
struct WorkState {
  using result_t = std::invoke_result_t<F>;
  using stored_t = std::conditional_t<std::is_void_v<result_t>, char, result_t>;
  enum Claim : int { kPending = 0, kComplete = 1, kCancelled = 2 };

  uv_work_t work{};
  F fn;
  std::unique_ptr<Rcvr> rcvr;
  std::optional<stored_t> value;
  std::exception_ptr error;
  std::atomic<int> claim{kPending};
  // Self-hold: keeps the state alive from uv_queue_work until after_work
  // (the opstate and the WorkStartNode may both be gone by then).
  std::shared_ptr<WorkState> self;

  explicit WorkState(F f) : fn(std::move(f)) {}

  static void on_work(uv_work_t* w) {
    auto* ws = static_cast<WorkState*>(w->data);
    try {
      if constexpr (std::is_void_v<result_t>) {
        ws->fn();
        ws->value.emplace(0);
      } else {
        ws->value.emplace(ws->fn());
      }
    } catch (...) {
      ws->error = std::current_exception();
    }
  }

  static void after_work(uv_work_t* w, int /*status*/) {
    auto* ws = static_cast<WorkState*>(w->data);
    int expected = kPending;
    if (!ws->claim.compare_exchange_strong(expected, kComplete)) {
      ws->self.reset();  // cancelled: drop the result
      return;
    }
    auto rcvr = std::move(ws->rcvr);
    auto value = std::move(ws->value);
    auto error = ws->error;
    ws->self.reset();
    if (error) {
      stdexec::set_error(std::move(*rcvr), error);
    } else if constexpr (std::is_void_v<result_t>) {
      stdexec::set_value(std::move(*rcvr));
    } else {
      stdexec::set_value(std::move(*rcvr), std::move(*value));
    }
  }
};

}  // namespace uv_detail

template <class F>
template <class Rcvr>
struct UvScheduler::work_sender<F>::opstate {
  using operation_state_concept = stdexec::operation_state_tag;

  std::shared_ptr<UvSchedState> st_;
  Rcvr rcvr_;
  F fn_;
  std::shared_ptr<uv_detail::WorkState<F, Rcvr>> ws_;

  opstate(std::shared_ptr<UvSchedState> st, Rcvr rcvr, F fn)
    : st_(std::move(st)), rcvr_(std::move(rcvr)), fn_(std::move(fn)) {}

  opstate(opstate&&) = delete;  // P2300: operation states are immovable
  opstate(const opstate&) = delete;
  opstate& operator=(const opstate&) = delete;

  ~opstate() {
    if (ws_) {
      int expected = uv_detail::WorkState<F, Rcvr>::kPending;
      ws_->claim.compare_exchange_strong(expected, uv_detail::WorkState<F, Rcvr>::kCancelled);
    }
  }

  void start() & noexcept {
    auto ws = std::make_shared<uv_detail::WorkState<F, Rcvr>>(std::move(fn_));
    ws->rcvr = std::make_unique<Rcvr>(std::move(rcvr_));
    ws->work.data = ws.get();
    ws_ = std::move(ws);

    bool closed = false;
    {
      std::lock_guard lock(st_->mu);
      closed = st_->closed;
      if (!closed) {
        // uv_queue_work must be called from the loop thread; hop through the queue.
        st_->push(new WorkStartNode(st_->loop, ws_));
      }
    }
    if (closed) {
      int expected = uv_detail::WorkState<F, Rcvr>::kPending;
      if (ws_->claim.compare_exchange_strong(expected,
                                             uv_detail::WorkState<F, Rcvr>::kCancelled)) {
        auto rcvr = std::move(ws_->rcvr);
        stdexec::set_error(std::move(*rcvr),
                           std::make_exception_ptr(std::runtime_error("uv worker stopped")));
      }
      return;
    }
    st_->wake();
  }

  // Queued start task: calls uv_queue_work on the loop thread.
  struct WorkStartNode : UvSchedState::StartNode {
    uv_loop_t* loop_;
    std::shared_ptr<uv_detail::WorkState<F, Rcvr>> ws_;
    WorkStartNode(uv_loop_t* loop, std::shared_ptr<uv_detail::WorkState<F, Rcvr>> ws)
      : loop_(loop), ws_(std::move(ws)) {}
    bool run() override {
      ws_->self = ws_;  // hold until after_work
      const int rc = uv_queue_work(loop_, &ws_->work,
                                   &uv_detail::WorkState<F, Rcvr>::on_work,
                                   &uv_detail::WorkState<F, Rcvr>::after_work);
      if (rc != 0) {
        // Failed to enqueue: complete with an error so the receiver never
        // hangs, then release the self-hold.
        int expected = uv_detail::WorkState<F, Rcvr>::kPending;
        if (ws_->claim.compare_exchange_strong(expected,
                                               uv_detail::WorkState<F, Rcvr>::kComplete)) {
          auto rcvr = std::move(ws_->rcvr);
          ws_->self.reset();
          stdexec::set_error(std::move(*rcvr),
                             std::make_exception_ptr(
                                 std::runtime_error("uv_queue_work failed")));
          return true;
        }
      }
      return true;
    }
  };
};

template <class F>
template <class Rcvr>
UvScheduler::work_sender<F>::opstate<Rcvr> UvScheduler::work_sender<F>::connect(Rcvr rcvr) && {
  return opstate<Rcvr>{st_, std::move(rcvr), std::move(fn_)};
}

}  // namespace foreign_demo
