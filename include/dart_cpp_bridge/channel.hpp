#pragma once

#include <async_simple/Executor.h>

#include <atomic>
#include <concepts>
#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

// Tokio-style mpsc/oneshot channels for C++20 coroutines (async_simple::Lazy).
//
// Thread-safe by default (mutex-protected shared state):
//
//   auto [tx, rx] = co::mpsc::unbounded<int>();
//   tx.send(1);                     // non-blocking, any thread, returns bool
//   auto v = co_await rx.recv();    // optional<T>; suspends if empty
//
//   auto [tx, rx] = co::oneshot::channel<int>();
//   tx.send(42);
//   auto v = co_await rx.recv();
//
// Send never blocks. Recv is non-blocking for the calling thread: it either
// returns immediately or suspends the coroutine until a value/close arrives.
//
// recv()/oneshot::recv() implement coAwait(Executor*) so that when used inside
// async_simple::Lazy with an executor (e.g. .via(&ex)), the waiter is resumed
// by scheduling back onto that executor. This avoids ViaAsyncAwaiter and the
// cross-thread destroy race on ViaCoroutine. Without an executor, the waiter
// is resumed directly on the sender's thread.
//
// mpsc is multi-producer, single-consumer. Do not call recv() concurrently.

namespace co {

template <typename T>
concept channel_value =
  std::movable<T> && !std::is_const_v<T> && !std::is_volatile_v<T>;

namespace detail {

inline void wake_waiter(
  std::coroutine_handle<> h,
  async_simple::Executor* ex
)
{
  if (!h) {
    return;
  }
  if (ex) {
    ex->schedule([h]() { h.resume(); });
  } else {
    h.resume();
  }
}

template <channel_value T>
struct mpsc_state {
  std::mutex mu;
  std::queue<T> queue;
  std::coroutine_handle<> waiter{};
  async_simple::Executor* waiter_ex = nullptr;
  std::atomic<int> senders{1};
  bool closed = false;

  // Caller must wake the returned handle outside the lock via wake_waiter.
  std::pair<std::coroutine_handle<>, async_simple::Executor*> close_locked()
  {
    if (closed) {
      return {{}, nullptr};
    }
    closed = true;
    auto h = std::exchange(waiter, {});
    auto* ex = std::exchange(waiter_ex, nullptr);
    return {h, ex};
  }

  void close()
  {
    std::coroutine_handle<> h;
    async_simple::Executor* ex = nullptr;
    {
      std::lock_guard lock(mu);
      std::tie(h, ex) = close_locked();
    }
    wake_waiter(h, ex);
  }

};

template <channel_value T>
struct oneshot_state {
  std::mutex mu;
  std::optional<T> value;
  std::coroutine_handle<> waiter{};
  async_simple::Executor* waiter_ex = nullptr;
  bool sent = false;
  bool closed = false;

  std::pair<std::coroutine_handle<>, async_simple::Executor*> close_locked()
  {
    if (closed || sent) {
      return {{}, nullptr};
    }
    closed = true;
    auto h = std::exchange(waiter, {});
    auto* ex = std::exchange(waiter_ex, nullptr);
    return {h, ex};
  }

  void close()
  {
    std::coroutine_handle<> h;
    async_simple::Executor* ex = nullptr;
    {
      std::lock_guard lock(mu);
      std::tie(h, ex) = close_locked();
    }
    wake_waiter(h, ex);
  }

};

template <channel_value T>
struct mpsc_recv_awaiter {
  mpsc_state<T>* st;
  async_simple::Executor* ex;

  bool await_ready() const noexcept
  {
    std::lock_guard lock(st->mu);
    return !st->queue.empty() || st->closed;
  }

  // false = do not suspend (value/close arrived between ready and here).
  bool await_suspend(std::coroutine_handle<> h) noexcept
  {
    std::lock_guard lock(st->mu);
    if (!st->queue.empty() || st->closed) {
      return false;
    }
    st->waiter = h;
    st->waiter_ex = ex;
    return true;
  }

  std::optional<T> await_resume()
  {
    std::lock_guard lock(st->mu);
    if (!st->queue.empty()) {
      T v = std::move(st->queue.front());
      st->queue.pop();
      return v;
    }
    return std::nullopt;
  }

};

template <channel_value T>
struct mpsc_recv_awaitable {
  mpsc_state<T>* st;

  // Prefer async_simple path: avoids ViaAsyncAwaiter cross-thread destroy race.
  auto coAwait(async_simple::Executor* ex) noexcept
  {
    return mpsc_recv_awaiter<T>{st, ex};
  }

  // Also act as a plain awaiter (ex = nullptr) so co_await works outside Lazy.
  bool await_ready() const noexcept
  {
    return mpsc_recv_awaiter<T>{st, nullptr}.await_ready();
  }

  bool await_suspend(std::coroutine_handle<> h) noexcept
  {
    return mpsc_recv_awaiter<T>{st, nullptr}.await_suspend(h);
  }

  std::optional<T> await_resume()
  {
    return mpsc_recv_awaiter<T>{st, nullptr}.await_resume();
  }

};

template <channel_value T>
struct oneshot_recv_awaiter {
  oneshot_state<T>* st;
  async_simple::Executor* ex;

  bool await_ready() const noexcept
  {
    std::lock_guard lock(st->mu);
    return st->sent || st->closed;
  }

  bool await_suspend(std::coroutine_handle<> h) noexcept
  {
    std::lock_guard lock(st->mu);
    if (st->sent || st->closed) {
      return false;
    }
    st->waiter = h;
    st->waiter_ex = ex;
    return true;
  }

  std::optional<T> await_resume()
  {
    std::lock_guard lock(st->mu);
    if (st->value) {
      auto v = std::move(*st->value);
      st->value.reset();
      return v;
    }
    return std::nullopt;
  }

};

template <channel_value T>
struct oneshot_recv_awaitable {
  oneshot_state<T>* st;

  auto coAwait(async_simple::Executor* ex) noexcept
  {
    return oneshot_recv_awaiter<T>{st, ex};
  }

  bool await_ready() const noexcept
  {
    return oneshot_recv_awaiter<T>{st, nullptr}.await_ready();
  }

  bool await_suspend(std::coroutine_handle<> h) noexcept
  {
    return oneshot_recv_awaiter<T>{st, nullptr}.await_suspend(h);
  }

  std::optional<T> await_resume()
  {
    return oneshot_recv_awaiter<T>{st, nullptr}.await_resume();
  }

};

}  // namespace detail

// ---------------------------------------------------------------------------
// mpsc::unbounded
// ---------------------------------------------------------------------------
namespace mpsc {

template <channel_value T>
class Receiver;

template <channel_value T>
class Sender {
 public:
  Sender() = default;
  explicit Sender(std::shared_ptr<detail::mpsc_state<T>> s)
    : state_(std::move(s)) {}

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
    std::coroutine_handle<> h;
    async_simple::Executor* ex = nullptr;
    {
      std::lock_guard lock(state_->mu);
      if (state_->closed) {
        return false;
      }
      state_->queue.push(std::move(value));
      h = std::exchange(state_->waiter, {});
      ex = std::exchange(state_->waiter_ex, nullptr);
    }
    detail::wake_waiter(h, ex);
    return true;
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
    std::lock_guard lock(state_->mu);
    return state_->closed;
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
      state_->close();
    }
    state_.reset();
  }

  std::shared_ptr<detail::mpsc_state<T>> state_;
};

template <channel_value T>
class Receiver {
 public:
  Receiver() = default;
  explicit Receiver(std::shared_ptr<detail::mpsc_state<T>> s)
    : state_(std::move(s)) {}

  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  Receiver(Receiver&& o) noexcept : state_(std::move(o.state_)) {}

  Receiver& operator=(Receiver&& o) noexcept
  {
    if (this != &o) {
      close_rx();
      state_ = std::move(o.state_);
    }
    return *this;
  }

  ~Receiver() { close_rx(); }

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  // co_await rx.recv() -> optional<T> (nullopt if closed & empty)
  auto recv() { return detail::mpsc_recv_awaitable<T>{state_.get()}; }

  std::optional<T> try_recv()
  {
    if (!state_) {
      return std::nullopt;
    }
    std::lock_guard lock(state_->mu);
    if (state_->queue.empty()) {
      return std::nullopt;
    }
    T v = std::move(state_->queue.front());
    state_->queue.pop();
    return v;
  }

  bool is_closed() const
  {
    if (!state_) {
      return true;
    }
    std::lock_guard lock(state_->mu);
    return state_->closed;
  }

 private:
  void close_rx()
  {
    if (state_) {
      state_->close();
      state_.reset();
    }
  }

  std::shared_ptr<detail::mpsc_state<T>> state_;
};

template <channel_value T>
struct Pair {
  Sender<T> tx;
  Receiver<T> rx;
};

template <channel_value T>
Pair<T> unbounded()
{
  auto st = std::make_shared<detail::mpsc_state<T>>();
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
class Sender {
 public:
  Sender() = default;
  explicit Sender(std::shared_ptr<detail::oneshot_state<T>> s)
    : state_(std::move(s)) {}

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

  // Non-blocking. Returns false if already sent, closed, or detached.
  bool send(T value)
  {
    if (!state_) {
      return false;
    }
    std::coroutine_handle<> h;
    async_simple::Executor* ex = nullptr;
    {
      std::lock_guard lock(state_->mu);
      if (state_->sent || state_->closed) {
        return false;
      }
      state_->sent = true;
      state_->value = std::move(value);
      h = std::exchange(state_->waiter, {});
      ex = std::exchange(state_->waiter_ex, nullptr);
    }
    state_.reset();
    detail::wake_waiter(h, ex);
    return true;
  }

  void close()
  {
    if (!state_) {
      return;
    }
    auto st = std::move(state_);
    st->close();
  }

 private:
  std::shared_ptr<detail::oneshot_state<T>> state_;
};

template <channel_value T>
class Receiver {
 public:
  Receiver() = default;
  explicit Receiver(std::shared_ptr<detail::oneshot_state<T>> s)
    : state_(std::move(s)) {}

  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;

  Receiver(Receiver&&) noexcept = default;
  Receiver& operator=(Receiver&&) noexcept = default;

  explicit operator bool() const {
    return static_cast<bool>(state_);
  }

  auto recv() { return detail::oneshot_recv_awaitable<T>{state_.get()}; }

  bool is_ready() const
  {
    if (!state_) {
      return true;
    }
    std::lock_guard lock(state_->mu);
    return state_->sent || state_->closed;
  }

 private:
  std::shared_ptr<detail::oneshot_state<T>> state_;
};

template <channel_value T>
struct Pair {
  Sender<T> tx;
  Receiver<T> rx;
};

template <channel_value T>
Pair<T> channel()
{
  auto st = std::make_shared<detail::oneshot_state<T>>();
  return {Sender<T>{st}, Receiver<T>{st}};
}

template <channel_value T>
Pair<T> channel(void* /*ioc*/)
{
  return channel<T>();
}

}  // namespace oneshot

}  // namespace co
