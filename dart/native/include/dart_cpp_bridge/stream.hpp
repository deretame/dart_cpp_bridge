#pragma once

#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_base.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

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

}  // namespace co::stream
