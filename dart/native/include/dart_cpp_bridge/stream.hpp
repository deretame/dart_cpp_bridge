#pragma once

#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_base.hpp"

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

// Interval/timer combinators for co::stream.
//
// These live in a separate header (rather than stream_base.hpp) because they
// call dcb::sleep, whose return type is `stdexec::sender auto` — a
// forward declaration is not enough to call it (clang rejects
// "function with deduced return type cannot be used before it is defined").
// dcb::sleep is defined in dart_cpp_bridge/runtime.hpp (it needs the Runtime
// singleton's io_context), which is included above.

namespace co::stream {

template <typename T>
Stream<T> Stream<T>::interval(std::chrono::milliseconds period)
{
  return interval_on(*dcb::Runtime::instance().io_scheduler(), period);
}

template <typename T>
template <typename Sched>
Stream<T> Stream<T>::interval_on(Sched sched, std::chrono::milliseconds period)
{
  static_assert(std::is_integral_v<T>, "interval requires integral type");
  struct Impl : StreamImpl<T> {
    Sched sched;
    std::chrono::milliseconds period;
    T counter = 0;
    Impl(Sched s, std::chrono::milliseconds p) : sched(std::move(s)), period(p) {}

    exec::task<std::optional<T>> next() override
    {
      co_await dcb::sleep(period, sched);
      co_return counter++;
    }

  };
  return Stream<T>(std::make_unique<Impl>(std::move(sched), period));
}

template <typename T>
Stream<T> interval(std::chrono::milliseconds period)
{
  return Stream<T>::interval(period);
}

template <typename T, typename Sched>
Stream<T> interval_on(Sched sched, std::chrono::milliseconds period)
{
  return Stream<T>::interval_on(std::move(sched), period);
}

// ---------------------------------------------------------------------------
// Concurrent merge
// ---------------------------------------------------------------------------

namespace detail {

// Multi-producer single-consumer rendezvous queue used by merge_concurrent.
// Producers are the per-source driver coroutines (running on the dcb runtime
// io thread, interleaved while their source streams await); the consumer is
// the merged stream's next(). Values are handed over directly when a waiter
// is parked, otherwise queued (unbounded).
template <typename T>
struct MergeQueue {
  std::mutex mu;
  std::deque<T> q;
  std::deque<co::oneshot::Sender<std::optional<T>>> waiters;
  std::size_t total = 0;
  std::size_t done = 0;

  void push(T v) {
    co::oneshot::Sender<std::optional<T>> tx;
    {
      std::lock_guard lock(mu);
      if (!waiters.empty()) {
        tx = std::move(waiters.front());
        waiters.pop_front();
      } else {
        q.push_back(std::move(v));
        return;
      }
    }
    tx.send(std::optional<T>{std::move(v)});
  }

  // One source ended. Wake one parked consumer so it re-checks (other
  // sources may still produce); the done counter is authoritative.
  void one_done() {
    co::oneshot::Sender<std::optional<T>> tx;
    {
      std::lock_guard lock(mu);
      ++done;
      if (!waiters.empty()) {
        tx = std::move(waiters.front());
        waiters.pop_front();
      }
    }
    if (tx) {
      tx.send(std::nullopt);
    }
  }

  exec::task<std::optional<T>> pop() {
    while (true) {
      {
        std::lock_guard lock(mu);
        if (!q.empty()) {
          auto v = std::move(q.front());
          q.pop_front();
          co_return v;
        }
        if (done == total) {
          co_return std::nullopt;
        }
      }
      // Park: register a waiter (double-checked under the lock in case a
      // push raced with our first check).
      auto [tx, rx] = co::oneshot::channel<std::optional<T>>();
      {
        std::lock_guard lock(mu);
        if (!q.empty()) {
          auto v = std::move(q.front());
          q.pop_front();
          co_return v;
        }
        if (done == total) {
          co_return std::nullopt;
        }
        waiters.push_back(std::move(tx));
      }
      auto m = co_await std::move(rx);  // optional<optional<T>>
      if (!m) {
        co_return std::nullopt;  // waiter sender destroyed (defensive)
      }
      if (m->has_value()) {
        co_return std::move(**m);
      }
      // Wake-up signal (a source ended): loop and re-check.
    }
  }
};

template <typename T>
exec::task<void> merge_driver(Stream<T> src, std::shared_ptr<MergeQueue<T>> st) {
  while (auto v = co_await src.next()) {
    st->push(std::move(*v));
  }
  st->one_done();
  co_return;
}

}  // namespace detail

template <typename T>
Stream<T> Stream<T>::merge_concurrent(std::vector<Stream<T>> sources)
{
  auto st = std::make_shared<detail::MergeQueue<T>>();
  for (auto& src : sources) {
    ++st->total;
    if (!src) {
      st->one_done();
      continue;
    }
    // Each source is pulled by its own driver on the dcb runtime io thread;
    // while a driver awaits (timer, channel, ...) the others advance, which
    // is what makes the merge concurrent.
    exec::start_detached(stdexec::starts_on(
        *dcb::Runtime::instance().io_scheduler(),
        detail::merge_driver<T>(std::move(src), st)));
  }

  struct Impl : StreamImpl<T> {
    std::shared_ptr<detail::MergeQueue<T>> st;
    explicit Impl(std::shared_ptr<detail::MergeQueue<T>> s) : st(std::move(s)) {}

    exec::task<std::optional<T>> next() override {
      co_return co_await st->pop();
    }
  };
  return Stream<T>(std::make_unique<Impl>(st));
}

template <typename T>
Stream<T> merge_concurrent(std::vector<Stream<T>> sources)
{
  return Stream<T>::merge_concurrent(std::move(sources));
}

}  // namespace co::stream
