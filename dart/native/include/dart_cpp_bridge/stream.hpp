#pragma once

#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_base.hpp"

#include <chrono>
#include <deque>
#include <exception>
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

    stdexec::task<std::optional<T>> next() override
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
  std::exception_ptr error;

  void push(T v) {
    // Keep ownership of the value until a parked receiver accepts it. A
    // receiver operation may be cancelled after its sender is removed from
    // the queue; oneshot::Sender::send(optional<T>&) reports that case and
    // leaves the value engaged so we can try the next waiter or enqueue it.
    std::optional<T> pending(std::move(v));
    while (pending) {
      co::oneshot::Sender<std::optional<T>> tx;
      {
        std::lock_guard lock(mu);
        if (error) {
          return;
        }
        while (!waiters.empty()) {
          tx = std::move(waiters.front());
          waiters.pop_front();
          if (!tx.receiver_detached()) {
            break;
          }
          tx = {};
        }
        if (!tx) {
          q.push_back(std::move(*pending));
          return;
        }
      }
      if (tx.send(pending)) {
        return;
      }
      // The receiver detached between the check above and send(). Retry with
      // the still-engaged value instead of silently dropping the source item.
    }
  }

  // One source ended. Wake one parked consumer so it re-checks (other
  // sources may still produce); the done counter is authoritative.
  void one_done() {
    co::oneshot::Sender<std::optional<T>> tx;
    {
      std::lock_guard lock(mu);
      if (error) {
        return;
      }
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

  void fail(std::exception_ptr e) {
    co::oneshot::Sender<std::optional<T>> tx;
    {
      std::lock_guard lock(mu);
      if (error) {
        return;
      }
      error = std::move(e);
      done = total;
      q.clear();
      if (!waiters.empty()) {
        tx = std::move(waiters.front());
        waiters.pop_front();
      }
      // Stream consumption is single-reader by contract. Close any
      // accidental additional waiters; the authoritative error is observed
      // by the next call to pop().
      waiters.clear();
    }
    if (tx) {
      try {
        tx.send(std::nullopt);
      } catch (...) {
        // The consumer may already have been destroyed; the stored error is
        // still available to a later next() call.
      }
    }
  }

  stdexec::task<std::optional<T>> pop() {
    while (true) {
      {
        std::lock_guard lock(mu);
        if (error) {
          std::rethrow_exception(error);
        }
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
        if (error) {
          std::rethrow_exception(error);
        }
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
stdexec::task<void> merge_driver(Stream<T> src, std::shared_ptr<MergeQueue<T>> st) {
  try {
    while (auto v = co_await src.next()) {
      st->push(std::move(*v));
    }
  } catch (...) {
    st->fail(std::current_exception());
    co_return;
  }
  st->one_done();
  co_return;
}

}  // namespace detail

template <typename T>
Stream<T> Stream<T>::merge_concurrent(std::vector<Stream<T>> sources)
{
  auto st = std::make_shared<detail::MergeQueue<T>>();
  std::vector<Stream<T>> active;
  active.reserve(sources.size());
  st->total = sources.size();
  for (auto& src : sources) {
    if (!src) {
      ++st->done;
      continue;
    }
    active.push_back(std::move(src));
  }
  // Publish the complete source count before any driver can run. Otherwise
  // an eager source may observe a partial total and finish the merged stream
  // while later source drivers are still being registered.
  for (auto& src : active) {
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

    stdexec::task<std::optional<T>> next() override {
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
