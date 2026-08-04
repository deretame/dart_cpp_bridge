#pragma once

#include "dart_cpp_bridge/stream.hpp"

#include <rigtorp/MPMCQueue.h>

#include <stdexec/execution.hpp>

#include <atomic>
#include <concepts>
#include <deque>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

// Tokio-style mpsc/oneshot channels built on std::exec (stdexec) senders.
//
// - Single-shot channels (oneshot): the receiver side is a *sender*. Its
//   completion signatures are:
//       set_value_t(std::optional<T>)   // value sent, or nullopt on close
//       set_error_t(std::exception_ptr) // send_error()
//   The completion fires on whichever thread calls send()/close()/send_error()
//   (or on the start() thread if the channel was already settled). Use
//   stdexec::continues_on() (or starts_on the io scheduler) to migrate
//   completion back to the io thread.
//
//   auto [tx, rx] = co::oneshot::channel<int>();
//   tx.send(1);                                    // non-blocking, any thread
//   auto v = co_await std::move(rx);               // optional<int> (in exec::task)
//   // or as a pure sender chain:
//   std::move(rx) | stdexec::then([](auto v) { ... }) | ...
//
// - Multi-shot channels (mpsc) use an asynchronous stream as their consumer
//   side: co::mpsc::Receiver<T> derives from co::stream::Stream<T>, so
//   map/filter/take and other combinators work directly on the receiver.
//
//   // unbounded (tokio::sync::mpsc::unbounded_channel): send() is a
//   // synchronous non-blocking call that never fails while the channel is
//   // open; the buffer grows without limit.
//   auto [tx, rx] = co::mpsc::unbounded<int>();
//   tx.send(1);                                       // non-blocking, any thread
//
//   // bounded (tokio::sync::mpsc::channel(capacity)): backpressure. send()
//   // is non-blocking on the calling thread, but when the channel is full
//   // the returned sender suspends until a receiver frees a slot; it
//   // completes with `true` once the value is accepted, `false` if the
//   // channel is closed (value dropped). Parking is cancel-safe
//   // (tokio-style): the value stays inside the operation state until a
//   // receiver claims it, so a stop request on the receiver's stop token
//   // (completion: set_stopped) — or destroying the operation state —
//   // withdraws the value instead of delivering it.
//   auto [btx, brx] = co::mpsc::bounded<int>(2);
//   auto ok = co_await btx.send(1);                   // true (slot free)
//   // ... capacity exhausted -> co_await btx.send(4) parks until a recv()
//   //
//   // receiving is identical for both kinds (and likewise honours the
//   // stop token of the awaiting receiver):
//   auto v = co_await rx.recv();                      // optional<T> (in exec::task)
//   auto vec = co_await std::move(rx).map(...).filter(...).collect();

namespace co {

template <typename T>
concept channel_value =
  std::movable<T> && !std::is_const_v<T> && !std::is_volatile_v<T>;

// ---------------------------------------------------------------------------
// oneshot
// ---------------------------------------------------------------------------
namespace oneshot {

template <channel_value T>
class Sender;

template <channel_value T>
class Receiver;

template <channel_value T>
struct Pair {
  Sender<T> tx;
  Receiver<T> rx;
};

template <channel_value T>
struct State {
  mutable std::mutex mu;
  enum class Status { kEmpty, kValue, kError } status{Status::kEmpty};
  std::optional<T> value;
  std::exception_ptr error;
  // Waiter opstate + completion callback, installed by the Receiver's opstate
  // on start(). `waiter` is non-owning: the opstate must outlive the
  // completion signal (P2300 guarantee).
  void* waiter{nullptr};
  using DeliverFn = void (*)(void* op, std::optional<T>&& value,
                             const std::exception_ptr& error, bool is_error);
  DeliverFn deliver{nullptr};
  // Set (under `mu`) when a parked waiter is destroyed before completing,
  // i.e. the receiver cancelled the wait. settle() then fails instead of
  // writing a value nobody will ever read.
  bool detached{false};
  std::atomic<bool> settled{false};
};

template <channel_value T>
class Sender {
 public:
  Sender() = default;

  explicit Sender(std::shared_ptr<State<T>> s) : state_(std::move(s)) {}

  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;

  Sender(Sender&& o) noexcept : state_(std::move(o.state_)) {}

  Sender& operator=(Sender&& o) noexcept
  {
    if (this != &o) {
      close();
      state_ = std::move(o.state_);
    }
    return *this;
  }

  ~Sender() { close(); }

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  // Non-blocking. Returns false if already sent/closed or detached.
  bool send(T value)
  {
    std::optional<T> wrapped(std::move(value));
    return settle(State<T>::Status::kValue, wrapped, nullptr);
  }

  // Non-blocking. Like send(), but on failure the value is left engaged in
  // `value` so the caller can reroute it (used by the mpsc direct hand-off,
  // which must not lose a value when a parked receiver cancels mid-hand-off).
  bool send(std::optional<T>& value)
  {
    return settle(State<T>::Status::kValue, value, nullptr);
  }

  // Non-blocking. Completes the receiver with an error. Returns false if
  // already settled or detached.
  bool send_error(std::exception_ptr ep)
  {
    std::optional<T> empty;
    return settle(State<T>::Status::kError, empty, std::move(ep));
  }

  void close()
  {
    std::optional<T> empty;
    (void)settle(State<T>::Status::kValue, empty, nullptr);
  }

  // True when the receiver side parked a waiter and then destroyed it
  // (cancelled) without completing: a send() to this channel would deliver
  // to nobody.
  bool receiver_detached() const
  {
    if (!state_) {
      return true;
    }
    std::lock_guard lock(state_->mu);
    return state_->detached;
  }

 private:
  friend class Receiver<T>;

  // `value` is moved into the state only on success; on failure (already
  // settled / detached) it is left untouched so the caller keeps the value.
  bool settle(typename State<T>::Status status, std::optional<T>& value,
              std::exception_ptr error)
  {
    if (!state_) {
      return false;
    }
    void* waiter = nullptr;
    typename State<T>::DeliverFn deliver = nullptr;
    {
      std::lock_guard lock(state_->mu);
      if (state_->status != State<T>::Status::kEmpty) {
        return false;  // already settled
      }
      if (state_->detached) {
        return false;  // the only waiter was cancelled mid-wait
      }
      // Write the payload first and publish `settled` last, all under `mu`:
      // a start() that observes settled==true while holding `mu` is then
      // guaranteed to also observe the payload. Publishing `settled` before
      // writing the payload (the previous CAS-then-lock order) let a
      // concurrent start() complete with an empty value.
      state_->status = status;
      state_->value = std::move(value);
      state_->error = std::move(error);
      state_->settled.store(true, std::memory_order_release);
      waiter = state_->waiter;
      deliver = state_->deliver;
      state_->waiter = nullptr;
      state_->deliver = nullptr;
    }
    if (waiter && deliver) {
      deliver(waiter, std::move(state_->value), state_->error,
              state_->status == State<T>::Status::kError);
    }
    state_.reset();
    return true;
  }

  std::shared_ptr<State<T>> state_;
};

template <channel_value T>
class Receiver {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures = stdexec::completion_signatures<
    stdexec::set_value_t(std::optional<T>),
    stdexec::set_error_t(std::exception_ptr)>;

  Receiver() = default;

  explicit Receiver(std::shared_ptr<State<T>> s) : state_(std::move(s)) {}

  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  Receiver(Receiver&&) noexcept = default;
  Receiver& operator=(Receiver&&) noexcept = default;

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  bool is_ready() const
  {
    if (!state_) {
      return true;
    }
    return state_->settled.load(std::memory_order_acquire);
  }

  // The receiver side IS the sender; connect it to a stdexec receiver.
  template <stdexec::receiver Rcvr>
  struct opstate {
    using operation_state_concept = stdexec::operation_state_tag;

    std::shared_ptr<State<T>> state_;
    Rcvr rcvr_;

    opstate(std::shared_ptr<State<T>> s, Rcvr rcvr)
      : state_(std::move(s)), rcvr_(std::move(rcvr)) {}

    opstate(opstate&& o) noexcept
      : state_(std::move(o.state_)), rcvr_(std::move(o.rcvr_)) {}

    opstate(const opstate&) = delete;
    opstate& operator=(const opstate&) = delete;

    ~opstate()
    {
      // Unregister the waiter so a late send()/close() never touches a
      // destroyed opstate, and mark the state detached so the late send()
      // fails instead of writing a value nobody will read.
      if (state_) {
        std::lock_guard lock(state_->mu);
        if (state_->waiter == this) {
          state_->waiter = nullptr;
          state_->deliver = nullptr;
          state_->detached = true;
        }
      }
    }

    static void deliver(void* op, std::optional<T>&& value,
                        const std::exception_ptr& error, bool is_error)
    {
      auto* self = static_cast<opstate*>(op);
      if (is_error) {
        stdexec::set_error(std::move(self->rcvr_), error);
      } else {
        stdexec::set_value(std::move(self->rcvr_), std::move(value));
      }
    }

    void start() noexcept
    {
      std::optional<T> value;
      std::exception_ptr error;
      bool is_error = false;
      {
        std::lock_guard lock(state_->mu);
        if (!state_->settled.load(std::memory_order_acquire)) {
          state_->waiter = this;
          state_->deliver = &opstate::deliver;
          return;
        }
        is_error = (state_->status == State<T>::Status::kError);
        if (is_error) {
          error = state_->error;
        } else {
          value = std::move(state_->value);
        }
      }
      // Complete outside `mu`: continuations may re-enter the channel.
      if (is_error) {
        stdexec::set_error(std::move(rcvr_), std::move(error));
      } else {
        stdexec::set_value(std::move(rcvr_), std::move(value));
      }
    }
  };

  template <stdexec::receiver Rcvr>
  opstate<Rcvr> connect(Rcvr rcvr) && {
    return opstate<Rcvr>(std::move(state_), std::move(rcvr));
  }

 private:
  std::shared_ptr<State<T>> state_;
};

template <channel_value T>
Pair<T> channel()
{
  auto st = std::make_shared<State<T>>();
  return {Sender<T>{st}, Receiver<T>{st}};
}

// Backward-compatible overload: previously took asio::io_context*; ignored.
template <channel_value T>
Pair<T> channel(void* /*ioc*/)
{
  return channel<T>();
}

}  // namespace oneshot

// ---------------------------------------------------------------------------
// mpsc::unbounded
// ---------------------------------------------------------------------------
namespace mpsc {

template <channel_value T>
class Receiver;

template <channel_value T>
class Sender;

template <channel_value T>
struct Pair {
  Sender<T> tx;
  Receiver<T> rx;
};

template <channel_value T>
class BoundedSender;

template <channel_value T>
struct BoundedPair {
  BoundedSender<T> tx;
  Receiver<T> rx;
};

template <channel_value T>
struct State {
  static constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();

  // Bounded channels: lock-free MPMC ring buffer (strict FIFO — the atomic
  // slot ticket assigns a global order, so values arrive exactly in send()
  // call order even across threads). Elements are wrapped in std::optional<T>
  // because the pop API needs a default-constructible target, while T itself
  // may be move-only / non-default-constructible. Not constructed when
  // capacity == 0 (rendezvous) or for unbounded channels.
  std::optional<rigtorp::MPMCQueue<std::optional<T>>> bq;
  // Unbounded channels: dynamically growing FIFO buffer (no fixed capacity
  // can exist).
  std::deque<T> uq;
  // Values buffered in `bq` (bounded channels only; used by
  // BoundedSender::remaining_capacity). Rendezvous hand-offs never touch it.
  std::atomic<std::size_t> count{0};
  // Configured capacity (bounded); kUnbounded for unbounded channels.
  std::size_t capacity{kUnbounded};
  bool bounded_mode{false};
  // Guards `uq`, `bq` state transitions, the parked-receiver slot, the
  // closed flag, and the parked-sender wait list.
  mutable std::mutex mu;
  // Type-erased wait node of a parked bounded send, embedded in the send
  // opstate (which lives on the caller's stack / coroutine frame — no
  // allocation). The parked value stays inside the node (tokio-style
  // ownership): it enters the channel only when a receiver claims the node,
  // and is withdrawn silently if the opstate is destroyed while queued.
  //
  // All node operations are arbitrated under `mu`; completion callbacks
  // (`complete`) always fire after `mu` has been released.
  struct SendNode {
    SendNode* prev = nullptr;
    SendNode* next = nullptr;
    // Arbitration stage (guarded by State::mu):
    //   kInit    — start() has not enqueued the node yet
    //   kQueued  — linked in the wait list; the value is owned by the node
    //   kClaimed — a receiver took the value (take_value ran); completion owed
    //   kClosed  — close() won; completion with false is owed
    //   kStopped — a stop request won; the callback unlinked and completed
    //   kGone    — the opstate dtor consumed the node; no completion
    enum class Stage : unsigned char {
      kInit, kQueued, kClaimed, kClosed, kStopped, kGone
    };
    Stage stage = Stage::kInit;
    // `mu` held; pre: stage == kQueued. Moves the parked value out and
    // switches to kClaimed. The caller runs complete(true) after releasing
    // `mu`.
    virtual T take_value() = 0;
    // `mu` NOT held; called exactly once by the claiming receiver
    // (accepted=true) or by close() (accepted=false, value dropped).
    virtual void complete(bool accepted) noexcept = 0;
  };
  // Type-erased wait node of the single parked receiver (single-consumer
  // contract), embedded in the recv opstate. Symmetric to SendNode.
  struct RecvNode {
    // Arbitration stage (guarded by State::mu):
    //   kInit    — start() has not taken the slot yet
    //   kQueued  — holds the slot; waiting for a value / close
    //   kClaimed — a sender stored a direct-delivery value (store ran);
    //              complete_value() is owed
    //   kClosed  — close() won; complete_closed() is owed
    //   kStopped — a stop request won; the callback cleared the slot and
    //              completed with set_stopped
    //   kGone    — the opstate dtor consumed the node; no completion
    enum class Stage : unsigned char {
      kInit, kQueued, kClaimed, kClosed, kStopped, kGone
    };
    Stage stage = Stage::kInit;
    // `mu` held; pre: stage == kQueued. Moves `v` into the node's storage
    // and switches to kClaimed. The caller runs complete_value() after
    // releasing `mu`.
    virtual void store(T&& v) = 0;
    // `mu` NOT held; pre: store() ran. Delivers set_value(optional<T>).
    virtual void complete_value() noexcept = 0;
    // `mu` NOT held. Delivers set_value(nullopt) for close().
    virtual void complete_closed() noexcept = 0;
  };
  // The single parked receiver (null while none). Invariant, under `mu`:
  // non-null ⟺ a live node in kQueued — the opstate dtor clears the slot
  // first, so a visible node is always deliverable (no stale-slot retries).
  RecvNode* recv_slot = nullptr;
  // FIFO wait list of parked bounded sends (intrusive; nodes live inside
  // their opstates).
  SendNode* ws_head = nullptr;
  SendNode* ws_tail = nullptr;
  std::atomic<int> senders{1};
  std::atomic<bool> closed{false};

  State() = default;

  explicit State(std::size_t cap)
    : capacity(cap), bounded_mode(true) {
    if (cap > 0) {
      bq.emplace(cap);
    }
  }

  // `mu` held.
  void queue_send(SendNode* n)
  {
    n->prev = ws_tail;
    n->next = nullptr;
    if (ws_tail) {
      ws_tail->next = n;
    } else {
      ws_head = n;
    }
    ws_tail = n;
  }

  // `mu` held.
  void unlink_send(SendNode* n)
  {
    if (n->prev) {
      n->prev->next = n->next;
    } else {
      ws_head = n->next;
    }
    if (n->next) {
      n->next->prev = n->prev;
    } else {
      ws_tail = n->prev;
    }
    n->prev = n->next = nullptr;
  }

  // Unbounded send: synchronous, non-blocking, never fails while open.
  bool try_send(T value)
  {
    if (closed.load(std::memory_order_acquire)) {
      return false;
    }
    RecvNode* slot = nullptr;
    {
      std::lock_guard lock(mu);
      if (closed.load(std::memory_order_relaxed)) {
        return false;
      }
      if (recv_slot != nullptr) {
        // Direct hand-off to the parked receiver (no buffering). Under `mu`
        // a visible slot node is always live (its dtor clears the slot
        // first), so the store cannot fail.
        slot = recv_slot;
        recv_slot = nullptr;
        slot->store(std::move(value));
      } else {
        // Buffered value and waiter state are both guarded by `mu`.
        uq.push_back(std::move(value));
      }
    }
    if (slot != nullptr) {
      slot->complete_value();
    }
    return true;
  }

  void close()
  {
    RecvNode* slot = nullptr;
    SendNode* pending = nullptr;
    {
      std::lock_guard lock(mu);
      bool expected = false;
      if (!closed.compare_exchange_strong(
        expected,
        true,
        std::memory_order_acq_rel,
        std::memory_order_relaxed)) {
        return;
      }
      slot = recv_slot;
      recv_slot = nullptr;
      if (slot != nullptr) {
        slot->stage = RecvNode::Stage::kClosed;
      }
      // Detach the whole wait list; parked sends complete with false (their
      // values are still owned by the nodes and are dropped with them).
      pending = ws_head;
      ws_head = ws_tail = nullptr;
      for (auto* n = pending; n != nullptr; n = n->next) {
        n->stage = SendNode::Stage::kClosed;
      }
    }
    if (slot != nullptr) {
      slot->complete_closed();
    }
    while (pending != nullptr) {
      auto* n = pending;
      pending = pending->next;
      n->prev = n->next = nullptr;
      n->complete(false);
    }
  }

  std::optional<T> try_recv()
  {
    std::optional<T> out;
    SendNode* claimed = nullptr;
    {
      std::lock_guard lock(mu);
      if (bounded_mode) {
        if (bq) {
          std::optional<T> slot;
          if (bq->try_pop(slot)) {
            out = std::move(slot);
            count.fetch_sub(1, std::memory_order_relaxed);
            if (ws_head != nullptr) {
              claimed = ws_head;
              unlink_send(claimed);
              bq->try_push(std::optional<T>(claimed->take_value()));
              count.fetch_add(1, std::memory_order_relaxed);
            }
          }
        }
        if (!out && ws_head != nullptr) {
          claimed = ws_head;
          unlink_send(claimed);
          out = claimed->take_value();
        }
      } else if (!uq.empty()) {
        out = std::move(uq.front());
        uq.pop_front();
      }
    }
    if (claimed != nullptr) {
      claimed->complete(true);
    }
    return out;
  }

  bool is_closed() const
  {
    return closed.load(std::memory_order_acquire);
  }
};

// Operation state of a bounded send. Immovable: the wait node is linked by
// address. The value stays in `value_` until a receiver claims it
// (tokio-style ownership) — cancelling the wait withdraws the value from the
// channel: either via a stop request on the receiver's token (completion:
// set_stopped) or by destroying the opstate while queued (no completion).
template <channel_value T, stdexec::receiver Rcvr>
class send_opstate : public State<T>::SendNode {
  using Node = typename State<T>::SendNode;

  struct stop_fn {
    send_opstate* self;
    void operator()() const noexcept { self->on_stop(); }
  };

  using stop_token_t = stdexec::stop_token_of_t<stdexec::env_of_t<Rcvr>>;
  using stop_callback_t = stdexec::stop_callback_for_t<stop_token_t, stop_fn>;

 public:
  using operation_state_concept = stdexec::operation_state_tag;

  send_opstate(std::shared_ptr<State<T>> st, T value, Rcvr rcvr)
    : state_(std::move(st)), value_(std::move(value)), rcvr_(std::move(rcvr)) {}

  send_opstate(const send_opstate&) = delete;
  send_opstate& operator=(const send_opstate&) = delete;

  ~send_opstate()
  {
    // Destroying a still-queued opstate cancels the wait: unlink so a late
    // claim/close never touches the node; the value is destroyed with the
    // opstate (withdrawn). Destroying a kClaimed node before its completion
    // fires is a P2300 contract violation (same guarantee level as every
    // stdexec algorithm).
    if (state_) {
      std::lock_guard lock(state_->mu);
      if (this->stage == Node::Stage::kQueued) {
        state_->unlink_send(this);
        this->stage = Node::Stage::kGone;
      }
    }
  }

  // State<T>::SendNode interface ---------------------------------------------
  T take_value() override
  {
    // `mu` held by the caller; pre: kQueued.
    this->stage = Node::Stage::kClaimed;
    return std::move(*value_);
  }

  void complete(bool accepted) noexcept override
  {
    // `mu` released by the caller. Unregister first (never while holding
    // `mu`): this may wait for an in-flight stop callback, which itself
    // needs `mu` — free here, so it observes kClaimed/kClosed and backs off.
    stop_reg_.reset();
    stdexec::set_value(std::move(rcvr_), accepted);
  }

  // ---------------------------------------------------------------------------
  void start() noexcept
  {
    auto* st = state_.get();
    if (!st) {
      stdexec::set_value(std::move(rcvr_), false);
      return;
    }
    // Register the stop callback before any completion can be in flight. A
    // stop that was already requested fires the callback inline here, which
    // (with the stage still kInit) just sets stop_requested_ for the
    // arbitration below.
    stop_reg_.emplace(stdexec::get_stop_token(stdexec::get_env(rcvr_)),
                      stop_fn{this});
    typename State<T>::RecvNode* direct = nullptr;
    enum class Outcome { kClosed, kDirect, kAccepted, kQueued, kStopped };
    Outcome outcome;
    {
      std::lock_guard lock(st->mu);
      if (stop_requested_) {
        outcome = Outcome::kStopped;
      } else if (st->closed.load(std::memory_order_relaxed)) {
        outcome = Outcome::kClosed;
      } else if (st->recv_slot != nullptr) {
        // Direct hand-off to the parked receiver (no buffering). Under `mu`
        // a visible slot node is always live (its dtor clears the slot
        // first), so the store cannot fail.
        direct = st->recv_slot;
        st->recv_slot = nullptr;
        direct->store(std::move(*value_));
        outcome = Outcome::kDirect;
      } else if (st->bq) {
        // Wrap first so a failed try_push does not consume the value
        // (the queue only consumes args on the success path).
        std::optional<T> wrapped(std::move(*value_));
        if (st->bq->try_push(std::move(wrapped))) {
          st->count.fetch_add(1, std::memory_order_relaxed);
          outcome = Outcome::kAccepted;
        } else {
          // Full: park until a receiver frees a slot.
          *value_ = std::move(*wrapped);
          this->stage = Node::Stage::kQueued;
          st->queue_send(this);
          outcome = Outcome::kQueued;
        }
      } else {
        // capacity == 0 (rendezvous): never buffers; park until a
        // receiver takes the value directly.
        this->stage = Node::Stage::kQueued;
        st->queue_send(this);
        outcome = Outcome::kQueued;
      }
    }
    // Complete outside `mu`: continuations may re-enter the channel. The
    // parked path keeps the stop registration armed; every other path
    // unregisters before completing (never while holding `mu`).
    if (outcome != Outcome::kQueued) {
      stop_reg_.reset();
    }
    switch (outcome) {
      case Outcome::kClosed:
        stdexec::set_value(std::move(rcvr_), false);
        return;
      case Outcome::kAccepted:
        stdexec::set_value(std::move(rcvr_), true);
        return;
      case Outcome::kStopped:
        stdexec::set_stopped(std::move(rcvr_));
        return;
      case Outcome::kQueued:
        return;  // parked; a receiver, close(), or stop completes the node
      case Outcome::kDirect:
        direct->complete_value();
        stdexec::set_value(std::move(rcvr_), true);
        return;
    }
  }

 private:
  // Stop callback body (fires on the stop-requesting thread). Arbitrates
  // under `mu`; the completion fires after `mu` is released.
  void on_stop() noexcept
  {
    auto* st = state_.get();
    if (!st) {
      return;
    }
    bool fire = false;
    {
      std::lock_guard lock(st->mu);
      if (this->stage == Node::Stage::kQueued) {
        // Withdraw the parked send: unlink and complete with set_stopped;
        // the value never enters the channel (tokio cancel-safety).
        st->unlink_send(this);
        this->stage = Node::Stage::kStopped;
        fire = true;
      } else if (this->stage == Node::Stage::kInit) {
        // Races start()'s arbitration: leave a flag; start() completes with
        // set_stopped when it observes this under `mu`.
        stop_requested_ = true;
      }
      // kClaimed / kClosed / kStopped / kGone: too late — the in-flight
      // completion wins (same precedence as tokio).
    }
    if (fire) {
      // After completing, the opstate must not be touched again.
      stdexec::set_stopped(std::move(rcvr_));
    }
  }

  std::shared_ptr<State<T>> state_;
  std::optional<T> value_;
  Rcvr rcvr_;
  bool stop_requested_ = false;  // guarded by state_->mu
  // Declared last so it is destroyed first: unregistering may block until an
  // in-flight callback returns, and the callback touches the members above.
  std::optional<stop_callback_t> stop_reg_;
};

// Sender returned by BoundedSender::send(). Carries the value until
// connect() moves it into the operation state; completes with `bool`
// (true = accepted, false = channel closed, value dropped).
template <channel_value T>
class send_sender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures = stdexec::completion_signatures<
    stdexec::set_value_t(bool),
    stdexec::set_stopped_t()>;

  send_sender(std::shared_ptr<State<T>> st, T value)
    : state_(std::move(st)), value_(std::move(value)) {}

  template <stdexec::receiver Rcvr>
  send_opstate<T, Rcvr> connect(Rcvr rcvr) && {
    return send_opstate<T, Rcvr>(std::move(state_), std::move(value_),
                                 std::move(rcvr));
  }

 private:
  std::shared_ptr<State<T>> state_;
  T value_;
};

// Operation state of a channel receive. Immovable: the wait node is linked
// by address. Parks by taking the state's single recv slot; destroying a
// still-queued opstate releases the slot (cancel).
template <channel_value T, stdexec::receiver Rcvr>
class recv_opstate : public State<T>::RecvNode {
  using Node = typename State<T>::RecvNode;

  struct stop_fn {
    recv_opstate* self;
    void operator()() const noexcept { self->on_stop(); }
  };

  using stop_token_t = stdexec::stop_token_of_t<stdexec::env_of_t<Rcvr>>;
  using stop_callback_t = stdexec::stop_callback_for_t<stop_token_t, stop_fn>;

 public:
  using operation_state_concept = stdexec::operation_state_tag;

  recv_opstate(std::shared_ptr<State<T>> st, Rcvr rcvr)
    : state_(std::move(st)), rcvr_(std::move(rcvr)) {}

  recv_opstate(const recv_opstate&) = delete;
  recv_opstate& operator=(const recv_opstate&) = delete;

  ~recv_opstate()
  {
    // Destroying a still-queued opstate cancels the wait: release the slot
    // so a late send/close never touches the node and a later recv() may
    // park normally. Destroying a kClaimed node before its completion fires
    // is a P2300 contract violation (same as every stdexec algorithm).
    if (state_) {
      std::lock_guard lock(state_->mu);
      if (this->stage == Node::Stage::kQueued) {
        state_->recv_slot = nullptr;
        this->stage = Node::Stage::kGone;
      }
    }
  }

  // State<T>::RecvNode interface ---------------------------------------------
  void store(T&& v) override
  {
    // `mu` held by the caller; pre: kQueued.
    held_ = std::move(v);
    this->stage = Node::Stage::kClaimed;
  }

  void complete_value() noexcept override
  {
    // `mu` released by the caller; pre: store() ran. Unregister the stop
    // callback first (never while holding `mu`; see send_opstate::complete).
    stop_reg_.reset();
    stdexec::set_value(std::move(rcvr_), std::move(held_));
  }

  void complete_closed() noexcept override
  {
    // `mu` released by the caller.
    stop_reg_.reset();
    stdexec::set_value(std::move(rcvr_), std::optional<T>());
  }

  // ---------------------------------------------------------------------------
  void start() noexcept
  {
    auto* st = state_.get();
    if (!st) {
      stdexec::set_value(std::move(rcvr_), std::optional<T>());
      return;
    }
    // Register the stop callback before any completion can be in flight (see
    // send_opstate::start).
    stop_reg_.emplace(stdexec::get_stop_token(stdexec::get_env(rcvr_)),
                      stop_fn{this});
    std::optional<T> out;
    typename State<T>::SendNode* claimed = nullptr;
    enum class Outcome { kValue, kClosed, kViolation, kQueued, kStopped };
    Outcome outcome;
    {
      std::lock_guard lock(st->mu);
      if (stop_requested_) {
        // Stop arrived before the arbitration; consume nothing.
        outcome = Outcome::kStopped;
      } else {
        if (st->bounded_mode) {
          if (st->bq) {
            // pop target is a default-constructible std::optional<T> (the
            // queue's element type); it is engaged iff a value was dequeued.
            std::optional<T> slot;
            if (st->bq->try_pop(slot)) {
              out = std::move(slot);
              st->count.fetch_sub(1, std::memory_order_relaxed);
              if (st->ws_head != nullptr) {
                // Free slot: accept the oldest parked value into the buffer
                // and wake its sender (outside the lock). try_push must
                // succeed: the slot was freed by this pop and all pushes
                // happen under `mu`.
                claimed = st->ws_head;
                st->unlink_send(claimed);
                st->bq->try_push(std::optional<T>(claimed->take_value()));
                st->count.fetch_add(1, std::memory_order_relaxed);
              }
            }
          }
          if (!out && st->ws_head != nullptr) {
            // Rendezvous (capacity == 0) or transient empty: deliver the
            // parked value directly, bypassing the buffer.
            claimed = st->ws_head;
            st->unlink_send(claimed);
            out = claimed->take_value();
          }
        } else if (!st->uq.empty()) {
          out = std::move(st->uq.front());
          st->uq.pop_front();
        }
        if (out) {
          outcome = Outcome::kValue;
        } else if (st->closed.load(std::memory_order_relaxed)) {
          outcome = Outcome::kClosed;
        } else if (st->recv_slot != nullptr) {
          // Single-consumer violation: a previous recv() is still parked and
          // alive. Fail the new recv() so the bug surfaces at the offending
          // call site instead of silently clobbering the parked wait.
          outcome = Outcome::kViolation;
        } else {
          this->stage = Node::Stage::kQueued;
          st->recv_slot = this;
          outcome = Outcome::kQueued;
        }
      }
    }
    // Complete outside `mu`: continuations may re-enter the channel. The
    // parked path keeps the stop registration armed; every other path
    // unregisters before completing (never while holding `mu`).
    if (claimed != nullptr) {
      claimed->complete(true);
    }
    if (outcome != Outcome::kQueued) {
      stop_reg_.reset();
    }
    switch (outcome) {
      case Outcome::kValue:
        stdexec::set_value(std::move(rcvr_), std::move(out));
        break;
      case Outcome::kClosed:
        stdexec::set_value(std::move(rcvr_), std::optional<T>());
        break;
      case Outcome::kViolation:
        stdexec::set_error(
            std::move(rcvr_),
            std::make_exception_ptr(std::logic_error(
                "co::mpsc: concurrent recv() on a single-consumer Receiver "
                "(a previous recv() is still pending)")));
        break;
      case Outcome::kStopped:
        stdexec::set_stopped(std::move(rcvr_));
        break;
      case Outcome::kQueued:
        break;  // parked; a sender, close(), or stop completes the node
    }
  }

 private:
  // Stop callback body (fires on the stop-requesting thread). Arbitrates
  // under `mu`; the completion fires after `mu` is released.
  void on_stop() noexcept
  {
    auto* st = state_.get();
    if (!st) {
      return;
    }
    bool fire = false;
    {
      std::lock_guard lock(st->mu);
      if (this->stage == Node::Stage::kQueued) {
        // Cancel the parked wait: release the slot and complete with
        // set_stopped; buffered values are untouched.
        st->recv_slot = nullptr;
        this->stage = Node::Stage::kStopped;
        fire = true;
      } else if (this->stage == Node::Stage::kInit) {
        // Races start()'s arbitration: leave a flag; start() completes with
        // set_stopped when it observes this under `mu`.
        stop_requested_ = true;
      }
      // kClaimed / kClosed / kStopped / kGone: too late — the in-flight
      // completion wins (same precedence as tokio).
    }
    if (fire) {
      // After completing, the opstate must not be touched again.
      stdexec::set_stopped(std::move(rcvr_));
    }
  }

  std::shared_ptr<State<T>> state_;
  Rcvr rcvr_;
  std::optional<T> held_;
  bool stop_requested_ = false;  // guarded by state_->mu
  // Declared last so it is destroyed first: unregistering may block until an
  // in-flight callback returns, and the callback touches the members above.
  std::optional<stop_callback_t> stop_reg_;
};

// Sender form of a channel receive (what Receiver::recv() awaits).
// Completes with std::optional<T> (disengaged = closed & drained), or
// set_error(logic_error) on a single-consumer violation.
template <channel_value T>
class recv_sender {
 public:
  using sender_concept = stdexec::sender_tag;
  using completion_signatures = stdexec::completion_signatures<
    stdexec::set_value_t(std::optional<T>),
    stdexec::set_error_t(std::exception_ptr),
    stdexec::set_stopped_t()>;

  explicit recv_sender(std::shared_ptr<State<T>> st) : state_(std::move(st)) {}

  template <stdexec::receiver Rcvr>
  recv_opstate<T, Rcvr> connect(Rcvr rcvr) && {
    return recv_opstate<T, Rcvr>(std::move(state_), std::move(rcvr));
  }

 private:
  std::shared_ptr<State<T>> state_;
};

template <channel_value T>
class Sender {
 public:
  Sender() = default;

  explicit Sender(std::shared_ptr<State<T>> s) : state_(std::move(s)) {}

  Sender(const Sender& o) : state_(o.state_)
  {
    if (state_) {
      state_->senders.fetch_add(1, std::memory_order_relaxed);
    }
  }

  Sender& operator=(const Sender& o)
  {
    if (this == &o) {
      return *this;
    }
    release();
    state_ = o.state_;
    if (state_) {
      state_->senders.fetch_add(1, std::memory_order_relaxed);
    }
    return *this;
  }

  Sender(Sender&& o) noexcept : state_(std::move(o.state_)) {}

  Sender& operator=(Sender&& o) noexcept
  {
    if (this == &o) {
      return *this;
    }
    release();
    state_ = std::move(o.state_);
    return *this;
  }

  ~Sender() { release(); }

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  // Non-blocking. Returns false if the channel is closed / detached.
  bool send(T value) const
  {
    if (!state_) {
      return false;
    }
    return state_->try_send(std::move(value));
  }

  void close() const
  {
    if (state_) {
      state_->close();
    }
  }

  bool is_closed() const
  {
    if (!state_) {
      return true;
    }
    return state_->is_closed();
  }

 private:
  void release()
  {
    if (!state_) {
      return;
    }
    bool last_sender =
      (state_->senders.fetch_sub(1, std::memory_order_acq_rel) == 1);
    if (last_sender) {
      close();
    }
    state_.reset();
  }

  std::shared_ptr<State<T>> state_;
};

// Bounded (backpressure) sender: `send()` never blocks a thread — when the
// channel is full the returned sender parks until a receiver frees a slot
// (tokio::sync::mpsc::Sender semantics). Completion: `true` once the value
// has been accepted, `false` if the channel is closed / detached (value
// dropped).
template <channel_value T>
class BoundedSender {
 public:
  BoundedSender() = default;

  explicit BoundedSender(std::shared_ptr<State<T>> s) : state_(std::move(s)) {}

  BoundedSender(const BoundedSender& o) : state_(o.state_)
  {
    if (state_) {
      state_->senders.fetch_add(1, std::memory_order_relaxed);
    }
  }

  BoundedSender& operator=(const BoundedSender& o)
  {
    if (this == &o) {
      return *this;
    }
    release();
    state_ = o.state_;
    if (state_) {
      state_->senders.fetch_add(1, std::memory_order_relaxed);
    }
    return *this;
  }

  BoundedSender(BoundedSender&& o) noexcept : state_(std::move(o.state_)) {}

  BoundedSender& operator=(BoundedSender&& o) noexcept
  {
    if (this == &o) {
      return *this;
    }
    release();
    state_ = std::move(o.state_);
    return *this;
  }

  ~BoundedSender() { release(); }

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  // Non-blocking, backpressure send. The returned sender completes with
  // `bool`: true = value accepted (immediately, or after a receiver freed a
  // slot), false = channel closed (value dropped). Cancel-safe (tokio-style):
  // the value stays inside the operation state until a receiver claims it, so
  // destroying the operation state while parked withdraws the value.
  send_sender<T> send(T value) const
  {
    // A detached sender (state_ == null) completes with false on start().
    return send_sender<T>(state_, std::move(value));
  }

  void close() const
  {
    if (state_) {
      state_->close();
    }
  }

  bool is_closed() const
  {
    if (!state_) {
      return true;
    }
    return state_->is_closed();
  }

  // Configured capacity (0 = rendezvous); 0 when detached.
  std::size_t capacity() const
  {
    if (!state_) {
      return 0;
    }
    return state_->capacity;
  }

  // Free slots right now. Note: this is a point-in-time snapshot; a
  // concurrent recv() may free more, a concurrent send() may consume them.
  std::size_t remaining_capacity() const
  {
    if (!state_) {
      return 0;
    }
    std::size_t n = state_->count.load(std::memory_order_acquire);
    return n >= state_->capacity ? 0 : state_->capacity - n;
  }

 private:
  void release()
  {
    if (!state_) {
      return;
    }
    bool last_sender =
      (state_->senders.fetch_sub(1, std::memory_order_acq_rel) == 1);
    if (last_sender) {
      close();
    }
    state_.reset();
  }

  std::shared_ptr<State<T>> state_;
};

template <channel_value T>
class Receiver : public co::stream::Stream<T> {
  struct ChannelStreamImpl : co::stream::StreamImpl<T> {
    std::shared_ptr<State<T>> state;
    explicit ChannelStreamImpl(std::shared_ptr<State<T>> s) : state(std::move(s)) {}

    ~ChannelStreamImpl()
    {
      if (state) {
        state->close();
      }
    }

    exec::task<std::optional<T>> next() override
    {
      if (!state) {
        co_return std::nullopt;
      }
      co_return co_await recv_sender<T>(state);
    }
  };

 public:
  Receiver() = default;

  explicit Receiver(std::shared_ptr<State<T>> s)
    : co::stream::Stream<T>(
      s ? std::make_unique<ChannelStreamImpl>(s) : nullptr),
    state_(std::move(s)) {}

  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  Receiver(Receiver&& o) noexcept
    : co::stream::Stream<T>(std::move(o)),
    state_(std::move(o.state_)) {}

  Receiver& operator=(Receiver&& o) noexcept
  {
    if (this != &o) {
      close_rx();
      co::stream::Stream<T>::operator=(std::move(o));
      state_ = std::move(o.state_);
    }
    return *this;
  }

  ~Receiver() { close_rx(); }

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  // co_await rx.recv() -> optional<T>; std::nullopt when closed & empty.
  // Single-consumer, enforced at runtime: calling recv() while a previous
  // recv() is still parked fails the new call with std::logic_error
  // (delivered as set_error) instead of silently clobbering the parked wait.
  exec::task<std::optional<T>> recv()
  {
    co_return co_await co::stream::Stream<T>::next();
  }

  // Raw sender form of recv() (advanced): the same receive operation without
  // the exec::task wrapper, for connecting to a custom receiver — e.g. one
  // whose environment carries a stop token.
  recv_sender<T> recv_raw()
  {
    return recv_sender<T>(state_);
  }

  std::optional<T> try_recv()
  {
    if (!state_) {
      return std::nullopt;
    }
    return state_->try_recv();
  }

  bool is_closed() const
  {
    if (!state_) {
      return true;
    }
    return state_->is_closed();
  }

 private:
  void close_rx()
  {
    if (state_) {
      state_->close();
      state_.reset();
    }
  }

  std::shared_ptr<State<T>> state_;
};

template <channel_value T>
Pair<T> unbounded()
{
  auto st = std::make_shared<State<T>>();
  return {Sender<T>{st}, Receiver<T>{st}};
}

// Backward-compatible overload: previously took asio::io_context*; ignored.
template <channel_value T>
Pair<T> unbounded(void* /*ioc*/)
{
  return unbounded<T>();
}

// Bounded channel with backpressure (tokio::sync::mpsc::channel semantics).
// `capacity == 0` makes it a rendezvous channel: every send parks until a
// receiver takes the value.
template <channel_value T>
BoundedPair<T> bounded(std::size_t capacity)
{
  auto st = std::make_shared<State<T>>(capacity);
  return {BoundedSender<T>{st}, Receiver<T>{st}};
}

// Backward-compatible overload: previously took asio::io_context*; ignored.
template <channel_value T>
BoundedPair<T> bounded(std::size_t capacity, void* /*ioc*/)
{
  return bounded<T>(capacity);
}

}  // namespace mpsc

}  // namespace co
