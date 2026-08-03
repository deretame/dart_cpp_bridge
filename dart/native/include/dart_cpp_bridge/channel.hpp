#pragma once

#include "dart_cpp_bridge/stream.hpp"

#include <async_simple/Executor.h>
#include <async_simple/Future.h>
#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>

#include <concurrentqueue.h>

#include <atomic>
#include <concepts>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

// Tokio-style mpsc/oneshot channels for C++20 coroutines (async_simple::Lazy).
//
// - Single-shot channels (oneshot) are implemented on top of
//   async_simple::Promise/Future.
// - Multi-shot channels (mpsc) use an asynchronous stream as their consumer side:
//   co::mpsc::Receiver<T> derives from co::stream::Stream<T>, so map/filter/take
//   and other combinators work directly on the receiver.
//
//   auto [tx, rx] = co::mpsc::unbounded<int>();
//   tx.send(1);                                       // non-blocking, any thread
//   auto v = co_await rx.recv();                      // optional<T>
//   auto vec = co_await std::move(rx).map(...).filter(...).collect();
//
//   auto [tx, rx] = co::oneshot::channel<int>();
//   tx.send(42);
//   auto v = co_await rx.recv();

namespace co {

template <typename T>
concept channel_value =
  std::movable<T> && !std::is_const_v<T> && !std::is_volatile_v<T>;

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
  // Guards the waiting-receiver Promise and the closed flag.
  mutable std::mutex mu;
  std::optional<async_simple::Promise<std::optional<T>>> waiter;
  std::atomic<int> senders{1};
  std::atomic<bool> closed{false};

  bool send(T value)
  {
    if (closed.load(std::memory_order_acquire)) {
      return false;
    }
    async_simple::Promise<std::optional<T>> promise;
    bool wake = false;
    {
      std::lock_guard lock(mu);
      if (closed.load(std::memory_order_relaxed)) {
        return false;
      }
      if (waiter) {
        promise = std::move(*waiter);
        waiter.reset();
        wake = true;
      }
    }
    if (wake) {
      promise.setValue(std::optional<T>(std::move(value)));
      return true;
    }
    queue.enqueue(std::move(value));
    return true;
  }

  void close()
  {
    std::optional<async_simple::Promise<std::optional<T>>> promise;
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
      promise = std::move(waiter);
      waiter.reset();
    }
    if (promise) {
      promise->setValue(std::optional<T>(std::nullopt));
    }
  }

  async_simple::Future<std::optional<T>> recv()
  {
    // Fast lock-free path first.
    T v;
    if (queue.try_dequeue(v)) {
      return async_simple::makeReadyFuture<std::optional<T>>(std::move(v));
    }
    {
      std::lock_guard lock(mu);
      if (queue.try_dequeue(v)) {
        return async_simple::makeReadyFuture<std::optional<T>>(std::move(v));
      }
      if (closed.load(std::memory_order_relaxed)) {
        return async_simple::makeReadyFuture<std::optional<T>>(std::nullopt);
      }
      waiter = async_simple::Promise<std::optional<T>>();
      return waiter->getFuture();
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

    async_simple::coro::Lazy<std::optional<T>> next() override
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
  async_simple::coro::Lazy<std::optional<T>> recv()
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

// ---------------------------------------------------------------------------
// oneshot
// ---------------------------------------------------------------------------
namespace oneshot {

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
  async_simple::Promise<std::optional<T>> promise;
  std::atomic<bool> settled{false};
  std::atomic<bool> taken{false};
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
    if (!state_) {
      return false;
    }
    bool expected = false;
    if (!state_->settled.compare_exchange_strong(expected, true)) {
      return false;
    }
    state_->promise.setValue(std::optional<T>(std::move(value)));
    state_.reset();
    return true;
  }

  void close()
  {
    if (!state_) {
      return;
    }
    bool expected = false;
    if (!state_->settled.compare_exchange_strong(expected, true)) {
      return;
    }
    state_->promise.setValue(std::optional<T>(std::nullopt));
    state_.reset();
  }

 private:
  std::shared_ptr<State<T>> state_;
};

template <channel_value T>
class Receiver {
 public:
  Receiver() = default;

  explicit Receiver(std::shared_ptr<State<T>> s) : state_(std::move(s)) {}

  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  Receiver(Receiver&&) noexcept = default;
  Receiver& operator=(Receiver&&) noexcept = default;

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  async_simple::Future<std::optional<T>> recv()
  {
    if (!state_) {
      return async_simple::makeReadyFuture<std::optional<T>>(std::nullopt);
    }
    bool expected = false;
    if (!state_->taken.compare_exchange_strong(expected, true)) {
      return async_simple::makeReadyFuture<std::optional<T>>(std::nullopt);
    }
    return state_->promise.getFuture();
  }

  bool is_ready() const
  {
    if (!state_) {
      return true;
    }
    return state_->settled.load(std::memory_order_acquire);
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

}  // namespace co
