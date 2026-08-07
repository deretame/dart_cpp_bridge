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

namespace demo {

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
  // Number of uv_queue_work operations started but not yet completed.
  // stop() waits for this to drain (and the queue to empty) before it stops
  // the loop: after_work needs a running loop, and uv_close/uv_loop_close on
  // an in-flight request is illegal.
  std::atomic<int> work_in_flight{0};

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
  // Lifecycle: start() pushes the node under mu; on_async pops the whole
  // queue and claims every node (kQueued → kRunning) inside the same
  // critical section as pop_all(). The opstate destructor claims under the
  // same lock, so it can only win the race while the node is still queued —
  // a node that lost the race was destroyed and must never run.
  // Heap-allocated nodes (TimerInitNode / TimerCancelNode / WorkStartNode)
  // free themselves at the end of run(); the embedded schedule opstate is
  // owned by its parent and must NOT be touched after run() returns (the
  // parent may be destroyed by the io thread as soon as set_value completes).
  struct StartNode : Node {
    enum Claim : int { kQueued = 0, kRunning = 1, kCancelled = 2 };
    std::atomic<int> claim{kQueued};
    virtual bool run() = 0;
    // Called when this node lost the claim race and will never run.
    // Heap-allocated nodes free themselves here; the embedded schedule
    // opstate overrides this to a no-op (its parent tree was destroyed and
    // the destructor already handled everything). Currently unreachable
    // (nothing marks a queued node cancelled while it is still queued) —
    // kept as forward-looking defense; runs outside st_->mu.
    virtual void on_discarded() noexcept { delete this; }
  };

  // Run on the loop thread; drains the start queue and runs each node.
  static void on_async(uv_async_t* h) {
    auto* st = static_cast<UvSchedState*>(h->data);
    Node* node;
    Node* dead = nullptr;
    {
      std::lock_guard lock(st->mu);
      node = st->pop_all();
      // Claim every node under the lock: the destructor claims (kQueued →
      // kCancelled) under the same lock, so it can never destroy a node that
      // is about to run. Cancelled nodes are dropped from the run list and
      // disposed of outside the lock.
      Node** pp = &node;
      while (*pp) {
        auto* start = static_cast<StartNode*>(*pp);
        int expected = StartNode::kQueued;
        if (start->claim.compare_exchange_strong(expected, StartNode::kRunning)) {
          pp = &start->next;
        } else {
          Node* d = *pp;
          *pp = d->next;
          d->next = dead;
          dead = d;
        }
      }
    }
    while (dead) {
      // Heap nodes free themselves (and undo queue-time bookkeeping) via
      // on_discarded(); the embedded schedule opstate no-ops. Today no node
      // can actually lose the claim (nothing marks a queued node cancelled
      // while it is still queued), so this is forward-looking defense.
      Node* next = dead->next;
      static_cast<StartNode*>(dead)->on_discarded();
      dead = next;
    }
    while (node) {
      Node* next = node->next;
      node->next = nullptr;
      static_cast<StartNode*>(node)->run();
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
    // Contract: the operation state must not be destroyed while its node is
    // claimed-but-not-yet-run (P2300 whole-tree destruction mid-flight).
    // All trees in this codebase are start_detached-owned and destroyed only
    // after completion, so do not place this sender in stop_when /
    // take_until-style trees that destroy started children.
    //
    // stdexec::task requires its start scheduler to be infallible (the
    // default task_scheduler cannot be constructed from a scheduler that may
    // send errors/stopped), so the loop-closed set_stopped branch — which
    // only fires on the worker-stop teardown race, when nothing can run
    // anyway — is mapped to a no-op set_value().
    return schedule_sender{st_} | stdexec::upon_stopped([]() noexcept {});
  }

  // Timer: completes with set_value() on the loop thread after `d` elapses.
  // Cancellable: a stop request wins the claim and completes with
  // set_stopped(); destroying the operation state cancels silently. A timer
  // still pending when the worker stops is completed with set_stopped() from
  // the close callback (see TimerState::on_close), so sender trees are never
  // leaked by stop().
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

      std::shared_ptr<UvSchedState> st_;
      // Receiver ownership: this opstate while the node is queued/unclaimed;
      // transferred to run() once on_async has claimed it (kRunning). The
      // destructor destroys it only on the cancellation path — never while
      // the node is claimed, because run() uses it.
      Rcvr* rcvr_;

      opstate(std::shared_ptr<UvSchedState> st, Rcvr rcvr)
        : st_(std::move(st)), rcvr_(new Rcvr(std::move(rcvr))) {}

      opstate(opstate&&) = delete;  // P2300: operation states are immovable
      opstate(const opstate&) = delete;
      opstate& operator=(const opstate&) = delete;

      ~opstate() {
        Rcvr* r = nullptr;
        {
          std::lock_guard lock(st_->mu);
          int expected = StartNode::kQueued;
          if (claim.compare_exchange_strong(expected, StartNode::kCancelled)) {
            // Still queued and unclaimed: cancel. Unlink so the loop never
            // runs a dead opstate, and destroy the receiver (outside the
            // lock: its destructor may re-enter the runtime).
            Node* prev = nullptr;
            Node* cur = st_->head;
            while (cur && cur != this) {
              prev = cur;
              cur = cur->next;
            }
            if (cur == this) {
              if (prev) {
                prev->next = cur->next;
              } else {
                st_->head = cur->next;
              }
              if (st_->tail == this) {
                st_->tail = prev;
              }
            }
            r = rcvr_;
            rcvr_ = nullptr;
          }
          // Claimed (kRunning): run() owns the receiver and completes it;
          // the destructor must not touch it. Destroying a
          // claimed-but-incomplete opstate is a whole-tree-destruction
          // scenario (P2300 out-of-contract) that this codebase never
          // exercises — starts_on trees are owned by start_detached and are
          // destroyed only after completion.
        }
        delete r;
      }

      void on_discarded() noexcept override {
        // The parent tree was destroyed while this node was still queued;
        // the destructor already unlinked it and destroyed the receiver.
        // Do not touch anything.
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
          int expected = StartNode::kQueued;
          if (claim.compare_exchange_strong(expected, StartNode::kCancelled)) {
            // Take the receiver into a local first: set_stopped may
            // reentrantly destroy this opstate (whole-tree destruction via
            // the completion chain), so nothing may be touched afterwards.
            Rcvr* r = rcvr_;
            rcvr_ = nullptr;
            stdexec::set_stopped(std::move(*r));
            delete r;
          }
          return;
        }
        st_->wake();
      }

      bool run() override {  // loop thread; claimed under mu by on_async
        if (claim.load(std::memory_order_acquire) != StartNode::kRunning) {
          return false;  // cancelled
        }
        Rcvr* r = rcvr_;
        if (!r) {
          return false;  // already completed via the closed path
        }
        stdexec::set_value(std::move(*r));
        delete r;
        return true;
      }

      // No dispose(): this opstate is embedded in the parent (starts_on) and
      // must not be touched after run() returns — the parent may already be
      // destroyed by the io thread at that point.
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
    int expected = kPending;
    if (ts->claim.compare_exchange_strong(expected, kStopped)) {
      // Nobody claimed this timer before it was closed: this is the
      // teardown path (UvWorker::stop() closes every remaining handle while
      // a schedule_after is still pending). Complete the receiver so the
      // owning sender tree is released instead of leaking.
      auto rcvr = std::move(ts->rcvr);
      ts->self.reset();
      stdexec::set_stopped(std::move(*rcvr));
      return;
    }
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
      delete this;
      return true;
    }
    uv_timer_start(&ts_->timer, &TimerState<Rcvr>::on_timer, /*timeout*/ 0, /*repeat*/ 0);
    delete this;
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
      delete this;
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
    delete this;
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
  // Back-pointer for the in-flight work counter (UvSchedState::work_in_flight);
  // stop() waits for the counter to reach zero before closing the loop.
  std::shared_ptr<UvSchedState> st;
  std::unique_ptr<Rcvr> rcvr;
  std::optional<stored_t> value;
  std::exception_ptr error;
  std::atomic<int> claim{kPending};
  // Self-hold: keeps the state alive from uv_queue_work until after_work
  // (the opstate and the WorkStartNode may both be gone by then).
  std::shared_ptr<WorkState> self;

  explicit WorkState(F f, std::shared_ptr<UvSchedState> s)
    : fn(std::move(f)), st(std::move(s)) {}

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
    if (ws->st) {
      ws->st->work_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    }
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
    auto ws = std::make_shared<uv_detail::WorkState<F, Rcvr>>(std::move(fn_), st_);
    ws->rcvr = std::make_unique<Rcvr>(std::move(rcvr_));
    ws->work.data = ws.get();
    ws_ = std::move(ws);

    bool closed = false;
    {
      std::lock_guard lock(st_->mu);
      closed = st_->closed;
      if (!closed) {
        // uv_queue_work must be called from the loop thread; hop through the
        // queue. Count the node at push time (under the lock) so stop() can
        // never observe an empty queue + zero counter while a work start is
        // still pending in a popped batch.
        st_->push(new WorkStartNode(st_->loop, ws_));
        st_->work_in_flight.fetch_add(1, std::memory_order_acq_rel);
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
      if (rc == 0) {
        // Counter was incremented at push time; after_work decrements it.
        delete this;
        return true;
      }
      // Failed to enqueue: undo the queue-time counter increment, complete
      // with an error so the receiver never hangs, then release the
      // self-hold (after_work will never run).
      ws_->st->work_in_flight.fetch_sub(1, std::memory_order_acq_rel);
      int expected = uv_detail::WorkState<F, Rcvr>::kPending;
      if (ws_->claim.compare_exchange_strong(expected,
                                             uv_detail::WorkState<F, Rcvr>::kComplete)) {
        auto rcvr = std::move(ws_->rcvr);
        ws_->self.reset();
        stdexec::set_error(std::move(*rcvr),
                           std::make_exception_ptr(
                               std::runtime_error("uv_queue_work failed")));
      } else {
        ws_->self.reset();  // already cancelled: drop the self-hold too
      }
      delete this;
      return true;
    }

    void on_discarded() noexcept override {
      // Never queued into uv: undo the push-time counter increment and free
      // the node. (The opstate's destructor already claimed the WorkState.)
      ws_->st->work_in_flight.fetch_sub(1, std::memory_order_acq_rel);
      delete this;
    }
  };
};

template <class F>
template <class Rcvr>
UvScheduler::work_sender<F>::opstate<Rcvr> UvScheduler::work_sender<F>::connect(Rcvr rcvr) && {
  return opstate<Rcvr>{st_, std::move(rcvr), std::move(fn_)};
}

}  // namespace demo
