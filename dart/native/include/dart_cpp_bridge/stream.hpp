#pragma once

#include <exec/task.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace dcb {
// Defined in runtime.hpp (needs the Runtime singleton's scheduler). Forward
// declaration so stream.hpp can use it without a runtime dependency.
template <typename Rep, typename Period>
stdexec::sender auto sleep(std::chrono::duration<Rep, Period> dur);
}  // namespace dcb

// Tokio-style asynchronous stream abstraction built on std::exec.
// Coroutine bodies are exec::task (which is itself a sender), so a Stream can
// be consumed with co_await or composed as a sender chain.
//
//   auto s = co::stream::from_vector({1, 2, 3})
//     .map([](int x) { return x * 2; })
//     .filter([](int x) { return x > 10; })
//     .take(5);
//
//   while (auto v = co_await s.next()) { ... }
//   // or
//   auto vec = co_await std::move(s).collect();
//
// Design:
// - Stream<T> is a type-erased, move-only handle to a StreamImpl<T>.
// - Combinators build a lazy wrapper chain; no background task is spawned
//   until the stream is actually consumed via next()/collect()/etc.
// - co::mpsc::Receiver<T> derives from Stream<T>; a multi-shot channel pipe
//   is therefore a Stream from the start.

namespace co::stream {

template <typename T>
class Stream;

template <typename T>
struct StreamImpl {
  virtual ~StreamImpl() = default;
  virtual exec::task<std::optional<T>> next() = 0;
};

template <typename T>
class Stream {
 public:
  using Item = T;

  Stream() = default;

  explicit Stream(std::unique_ptr<StreamImpl<T>> impl) : impl_(std::move(impl)) {}

  Stream(Stream&&) noexcept = default;
  Stream& operator=(Stream&&) noexcept = default;

  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  explicit operator bool() const noexcept { return impl_ != nullptr; }

  // Core async pull. Returns std::nullopt when the stream ends.
  exec::task<std::optional<T>> next()
  {
    if (!impl_) {
      co_return std::nullopt;
    }
    co_return co_await impl_->next();
  }

  // ---------------------------------------------------------------------------
  // Factories
  // ---------------------------------------------------------------------------

  // Stream from an existing vector (eager materialized values).
  static Stream<T> from_vector(std::vector<T> values)
  {
    struct Impl : StreamImpl<T> {
      std::vector<T> values;
      std::size_t pos = 0;
      explicit Impl(std::vector<T> v) : values(std::move(v)) {}

      exec::task<std::optional<T>> next() override
      {
        if (pos >= values.size()) {
          co_return std::nullopt;
        }
        co_return std::move(values[pos++]);
      }

    };
    return Stream<T>(std::make_unique<Impl>(std::move(values)));
  }

  // Stream that yields value every `period` via dcb::sleep. The yielded
  // values are 0, 1, 2, ...
  static Stream<T> interval(std::chrono::milliseconds period)
  {
    static_assert(std::is_integral_v<T>, "interval requires integral type");
    struct Impl : StreamImpl<T> {
      std::chrono::milliseconds period;
      T counter = 0;
      explicit Impl(std::chrono::milliseconds p) : period(p) {}

      exec::task<std::optional<T>> next() override
      {
        co_await dcb::sleep(period);
        co_return counter++;
      }

    };
    return Stream<T>(std::make_unique<Impl>(period));
  }

  static Stream<T> once(T value)
  {
    return from_vector(std::vector<T>{std::move(value)});
  }

  static Stream<T> empty() { return Stream<T>(); }

  // ---------------------------------------------------------------------------
  // Combinators (all consume the current stream and return a new one)
  // ---------------------------------------------------------------------------

  template <typename F>
  auto map(F&& f) &&->Stream<std::invoke_result_t<F, T>>
  {
    using U = std::invoke_result_t<F, T>;
    struct Impl : StreamImpl<U> {
      Stream<T> upstream;
      std::decay_t<F> f;

      Impl(Stream<T> s, F fn) : upstream(std::move(s)), f(std::forward<F>(fn)) {}

      exec::task<std::optional<U>> next() override
      {
        auto v = co_await upstream.next();
        if (!v) {
          co_return std::nullopt;
        }
        co_return f(std::move(*v));
      }

    };
    return Stream<U>(std::make_unique<Impl>(std::move(*this), std::forward<F>(f)));
  }

  template <typename F>
  Stream<T> filter(F&& f) &&
  {
    struct Impl : StreamImpl<T> {
      Stream<T> upstream;
      std::decay_t<F> f;

      Impl(Stream<T> s, F fn) : upstream(std::move(s)), f(std::forward<F>(fn)) {}

      exec::task<std::optional<T>> next() override
      {
        while (true) {
          auto v = co_await upstream.next();
          if (!v) {
            co_return std::nullopt;
          }
          if (f(*v)) {
            co_return v;
          }
        }
      }

    };
    return Stream<T>(std::make_unique<Impl>(std::move(*this), std::forward<F>(f)));
  }

  // flat_map: F returns Stream<U> for each T.
  template <typename F>
  auto flat_map(F&& f) &&->Stream<typename std::invoke_result_t<F, T>::Item>
  {
    using U = typename std::invoke_result_t<F, T>::Item;
    struct Impl : StreamImpl<U> {
      Stream<T> upstream;
      std::decay_t<F> f;
      std::optional<Stream<U>> current;

      Impl(Stream<T> s, F fn) : upstream(std::move(s)), f(std::forward<F>(fn)) {}

      exec::task<std::optional<U>> next() override
      {
        while (true) {
          if (current) {
            auto v = co_await current->next();
            if (v) {
              co_return v;
            }
            current.reset();
          }

          auto v = co_await upstream.next();
          if (!v) {
            co_return std::nullopt;
          }
          current = f(std::move(*v));
        }
      }

    };
    return Stream<U>(std::make_unique<Impl>(std::move(*this), std::forward<F>(f)));
  }

  Stream<T> take(std::size_t n) &&
  {
    struct Impl : StreamImpl<T> {
      Stream<T> upstream;
      std::size_t remaining;

      Impl(Stream<T> s, std::size_t n) : upstream(std::move(s)), remaining(n) {}

      exec::task<std::optional<T>> next() override
      {
        if (remaining == 0) {
          co_return std::nullopt;
        }
        --remaining;
        co_return co_await upstream.next();
      }

    };
    return Stream<T>(std::make_unique<Impl>(std::move(*this), n));
  }

  Stream<T> skip(std::size_t n) &&
  {
    struct Impl : StreamImpl<T> {
      Stream<T> upstream;
      std::size_t remaining;
      bool skipped = false;

      Impl(Stream<T> s, std::size_t n) : upstream(std::move(s)), remaining(n) {}

      exec::task<std::optional<T>> next() override
      {
        if (!skipped) {
          skipped = true;
          for (std::size_t i = 0; i < remaining; ++i) {
            auto v = co_await upstream.next();
            if (!v) {
              co_return std::nullopt;
            }
          }
        }
        co_return co_await upstream.next();
      }

    };
    return Stream<T>(std::make_unique<Impl>(std::move(*this), n));
  }

  template <typename F>
  Stream<T> take_while(F&& f) &&
  {
    struct Impl : StreamImpl<T> {
      Stream<T> upstream;
      std::decay_t<F> f;
      bool done = false;

      Impl(Stream<T> s, F fn) : upstream(std::move(s)), f(std::forward<F>(fn)) {}

      exec::task<std::optional<T>> next() override
      {
        if (done) {
          co_return std::nullopt;
        }
        auto v = co_await upstream.next();
        if (!v || !f(*v)) {
          done = true;
          co_return std::nullopt;
        }
        co_return v;
      }

    };
    return Stream<T>(std::make_unique<Impl>(std::move(*this), std::forward<F>(f)));
  }

  template <typename F>
  Stream<T> skip_while(F&& f) &&
  {
    struct Impl : StreamImpl<T> {
      Stream<T> upstream;
      std::decay_t<F> f;
      bool skipping = true;

      Impl(Stream<T> s, F fn) : upstream(std::move(s)), f(std::forward<F>(fn)) {}

      exec::task<std::optional<T>> next() override
      {
        if (skipping) {
          skipping = false;
          while (true) {
            auto v = co_await upstream.next();
            if (!v) {
              co_return std::nullopt;
            }
            if (!f(*v)) {
              co_return v;
            }
          }
        }
        co_return co_await upstream.next();
      }

    };
    return Stream<T>(std::make_unique<Impl>(std::move(*this), std::forward<F>(f)));
  }

  // Scan: stateful fold that emits every intermediate accumulator.
  template <typename Acc, typename F>
  Stream<Acc> scan(Acc init, F&& f) &&
  {
    struct Impl : StreamImpl<Acc> {
      Stream<T> upstream;
      Acc acc;
      std::decay_t<F> f;

      Impl(Stream<T> s, Acc a, F fn)
        : upstream(std::move(s)), acc(std::move(a)), f(std::forward<F>(fn))
      {}

      exec::task<std::optional<Acc>> next() override
      {
        auto v = co_await upstream.next();
        if (!v) {
          co_return std::nullopt;
        }
        acc = f(std::move(acc), std::move(*v));
        co_return acc;
      }

    };
    return Stream<Acc>(
      std::make_unique<Impl>(std::move(*this), std::move(init), std::forward<F>(f)));
  }

  // Zip with another stream. Yields pairs until either stream ends.
  template <typename U>
  Stream<std::pair<T, U>> zip(Stream<U> other) &&
  {
    struct Impl : StreamImpl<std::pair<T, U>> {
      Stream<T> first;
      Stream<U> second;

      Impl(Stream<T> a, Stream<U> b) : first(std::move(a)), second(std::move(b)) {}

      exec::task<std::optional<std::pair<T, U>>> next() override
      {
        auto a = co_await first.next();
        if (!a) {
          co_return std::nullopt;
        }
        auto b = co_await second.next();
        if (!b) {
          co_return std::nullopt;
        }
        co_return std::pair<T, U>(std::move(*a), std::move(*b));
      }

    };
    return Stream<std::pair<T, U>>(
      std::make_unique<Impl>(std::move(*this), std::move(other)));
  }

  // Merge two streams of the same type. Yields whichever stream produces next;
  // order between the two is not deterministic.
  // Note: this sequentializes pulls (first upstream then second). For true
  // concurrent merge, use a channel-based approach. This is sufficient for
  // single-threaded io_context usage.
  Stream<T> merge(Stream<T> other) &&
  {
    struct Impl : StreamImpl<T> {
      Stream<T> first;
      Stream<T> second;
      bool pull_first = true;

      Impl(Stream<T> a, Stream<T> b) : first(std::move(a)), second(std::move(b)) {}

      exec::task<std::optional<T>> next() override
      {
        // Alternate between streams to avoid starvation.
        pull_first = !pull_first;
        auto* primary = pull_first ? &first : &second;
        auto* fallback = pull_first ? &second : &first;

        auto v = co_await primary->next();
        if (v) {
          co_return v;
        }
        co_return co_await fallback->next();
      }

    };
    return Stream<T>(std::make_unique<Impl>(std::move(*this), std::move(other)));
  }

  // ---------------------------------------------------------------------------
  // Terminators
  // ---------------------------------------------------------------------------

  exec::task<std::vector<T>> collect()
  {
    std::vector<T> out;
    while (auto v = co_await next()) {
      out.push_back(std::move(*v));
    }
    co_return out;
  }

  template <typename F>
  exec::task<void> for_each(F&& f)
  {
    while (auto v = co_await next()) {
      f(std::move(*v));
    }
    co_return;
  }

  exec::task<std::optional<T>> first() { co_return co_await next(); }

  exec::task<std::size_t> count()
  {
    std::size_t n = 0;
    while (auto v = co_await next()) {
      (void)v;
      ++n;
    }
    co_return n;
  }

  template <typename Acc, typename F>
  exec::task<Acc> fold(Acc init, F&& f)
  {
    Acc acc = std::move(init);
    while (auto v = co_await next()) {
      acc = f(std::move(acc), std::move(*v));
    }
    co_return acc;
  }

 protected:
  // For derived classes (e.g. co::mpsc::Receiver) that want to supply their own
  // StreamImpl while keeping additional methods.
  std::unique_ptr<StreamImpl<T>>& impl() { return impl_; }

 private:
  std::unique_ptr<StreamImpl<T>> impl_;
};

// ---------------------------------------------------------------------------
// Free factories
// ---------------------------------------------------------------------------

template <typename T>
Stream<T> from_vector(std::vector<T> values)
{
  return Stream<T>::from_vector(std::move(values));
}

template <typename T>
Stream<T> interval(std::chrono::milliseconds period)
{
  return Stream<T>::interval(period);
}

template <typename T>
Stream<T> once(T value)
{
  return Stream<T>::once(std::move(value));
}

template <typename T>
Stream<T> empty()
{
  return Stream<T>::empty();
}

}  // namespace co::stream
