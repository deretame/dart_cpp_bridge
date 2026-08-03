#pragma once

#include "dart_cpp_bridge/stream.hpp"

#include <stdexec/execution.hpp>

#include <concurrentqueue.h>

#include <atomic>
#include <concepts>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
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
//   auto [tx, rx] = co::mpsc::unbounded<int>();
//   tx.send(1);                                       // non-blocking, any thread
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
    return settle(State<T>::Status::kValue, std::move(value), nullptr);
  }

  // Non-blocking. Completes the receiver with an error. Returns false if
  // already settled or detached.
  bool send_error(std::exception_ptr ep)
  {
    return settle(State<T>::Status::kError, std::nullopt, std::move(ep));
  }

  void close()
  {
    (void)settle(State<T>::Status::kValue, std::nullopt, nullptr);
  }

 private:
  friend class Receiver<T>;

  bool settle(typename State<T>::Status status, std::optional<T> value,
              std::exception_ptr error)
  {
    if (!state_) {
      return false;
    }
    bool expected = false;
    if (!state_->settled.compare_exchange_strong(expected, true)) {
      return false;
    }
    void* waiter = nullptr;
    typename State<T>::DeliverFn deliver = nullptr;
    {
      std::lock_guard lock(state_->mu);
      state_->status = status;
      state_->value = std::move(value);
      state_->error = std::move(error);
      waiter = state_->waiter;
      deliver = state_->deliver;
      state_->waiter = nullptr;
      state_->deliver = nullptr;
    }
    if (waiter && deliver) {
      std::fprintf(stderr, "[dcb-diag] settle: delivering to waiter=%p\n", waiter);
      deliver(waiter, std::move(state_->value), state_->error,
              state_->status == State<T>::Status::kError);
    } else {
      std::fprintf(stderr, "[dcb-diag] settle: no waiter (waiter=%p deliver=%p)\n",
                   (void*)waiter, (void*)deliver);
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
      // destroyed opstate.
      if (state_ && state_->waiter == this) {
        std::lock_guard lock(state_->mu);
        if (state_->waiter == this) {
          state_->waiter = nullptr;
          state_->deliver = nullptr;
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
      std::lock_guard lock(state_->mu);
      if (state_->settled.load(std::memory_order_relaxed)) {
        if (state_->status == State<T>::Status::kError) {
          auto ep = state_->error;
          stdexec::set_error(std::move(rcvr_), ep);
        } else {
          stdexec::set_value(std::move(rcvr_), std::move(state_->value));
        }
        return;
      }
      state_->waiter = this;
      state_->deliver = &opstate::deliver;
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
struct State {
  // Lock-free multi-producer queue for buffered values.
  moodycamel::ConcurrentQueue<T> queue;
  // Guards the waiting-receiver oneshot tx and the closed flag.
  mutable std::mutex mu;
  std::optional<oneshot::Sender<std::optional<T>>> pending_tx;
  std::atomic<int> senders{1};
  std::atomic<bool> closed{false};

  bool send(T value)
  {
    if (closed.load(std::memory_order_acquire)) {
      return false;
    }
    oneshot::Sender<std::optional<T>> tx;
    bool wake = false;
    {
      std::lock_guard lock(mu);
      if (closed.load(std::memory_order_relaxed)) {
        return false;
      }
      if (pending_tx) {
        tx = std::move(*pending_tx);
        pending_tx.reset();
        wake = true;
      }
    }
    if (wake) {
      return tx.send(std::optional<T>(std::move(value)));
    }
    queue.enqueue(std::move(value));
    return true;
  }

  void close()
  {
    oneshot::Sender<std::optional<T>> tx;
    {
      std::lock_guard lock(mu);
      bool expected = false;
      if (!closed.compare_exchange_strong(
        expected,
        true,
        std::memory_order_release,
        std::memory_order_relaxed)) {
        return;
      }
      if (pending_tx) {
        tx = std::move(*pending_tx);
        pending_tx.reset();
      }
    }
    if (tx) {
      tx.close();
    }
  }

  stdexec::sender auto recv()
  {
    // Fast lock-free path first.
    T v;
    if (queue.try_dequeue(v)) {
      return ready(std::move(v));
    }
    {
      std::lock_guard lock(mu);
      if (queue.try_dequeue(v)) {
        return ready(std::move(v));
      }
      if (closed.load(std::memory_order_relaxed)) {
        return ready_closed();
      }
      auto [tx, rx] = oneshot::channel<std::optional<T>>();
      pending_tx = std::move(tx);
      return std::move(rx);
    }
  }

  std::optional<T> try_recv()
  {
    T v;
    if (queue.try_dequeue(v)) {
      return v;
    }
    return std::nullopt;
  }

  bool is_closed() const
  {
    return closed.load(std::memory_order_acquire);
  }

 private:
  stdexec::sender auto ready(T v)
  {
    auto [tx, rx] = oneshot::channel<std::optional<T>>();
    tx.send(std::optional<T>(std::move(v)));
    return std::move(rx);
  }

  stdexec::sender auto ready_closed()
  {
    auto [tx, rx] = oneshot::channel<std::optional<T>>();
    tx.close();
    return std::move(rx);
  }
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
    return state_->send(std::move(value));
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
      co_return co_await state->recv();
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
  exec::task<std::optional<T>> recv()
  {
    co_return co_await co::stream::Stream<T>::next();
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

}  // namespace mpsc

}  // namespace co
