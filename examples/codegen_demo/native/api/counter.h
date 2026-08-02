#pragma once

#include "dart_cpp_bridge/annotate.h"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace demo::api {

class BRIDGE_OPAQUE Counter {
 public:
  BRIDGE_CONSTRUCTOR Counter(std::int32_t initialValue);
  BRIDGE_CONSTRUCTOR Counter();

  /// Destructor: marked with BRIDGE_DESTRUCTOR to document intent.
  /// Codegen does not generate a wire method for it — destruction is
  /// triggered by dcb_drop_object (Dart dispose / NativeFinalizer /
  /// session close), which releases the shared_ptr and invokes this
  /// destructor when the reference count reaches zero.
  /// Put custom cleanup logic here (close files, free resources, etc.).
  BRIDGE_DESTRUCTOR ~Counter();

  BRIDGE_ASYNC async_simple::coro::Lazy<std::int32_t> value() const;
  BRIDGE_SYNC std::int32_t valueSync() const;

  /// Designated toString: codegen turns this into the Dart `toString()`
  /// override on the opaque wrapper (sync wire call).
  BRIDGE_TO_STRING std::string toString() const;

  BRIDGE_ASYNC async_simple::coro::Lazy<void> increment(std::int32_t delta = 1);
  BRIDGE_NORMAL std::int32_t sleepAndGet(std::int32_t sleepMs);
  BRIDGE_ASYNC async_simple::coro::Lazy<std::int32_t> addList(
      const std::vector<std::int32_t>& values);
  BRIDGE_ASYNC async_simple::coro::Lazy<void> setValue(
      std::optional<std::int32_t> value);
  BRIDGE_ASYNC async_simple::coro::Lazy<Counter> duplicate() const;

  BRIDGE_ASYNC async_simple::coro::Lazy<std::int32_t> addTo(
      const Counter& other);

  static BRIDGE_SYNC std::int32_t sum(std::int32_t a, std::int32_t b);

  BRIDGE_ASYNC async_simple::coro::Lazy<std::string> greetDartFn(
      dcb::DartFn<std::string(std::string)> callback, std::string name);

  BRIDGE_NORMAL void tickStream(dcb::StreamSink<std::int32_t> sink,
                                std::int32_t count = 5,
                                std::int32_t intervalMs = 10);

 private:
  std::int32_t value_ = 0;
};

/// Free function: sum two Counter values (opaque as parameter, borrow semantics).
BRIDGE_ASYNC async_simple::coro::Lazy<std::int32_t> addCounters(
    const Counter& a, const Counter& b);

/// Free function: create a new Counter with source's value + offset
/// (opaque param → new opaque return, clone semantics).
BRIDGE_SYNC Counter cloneWithOffset(const Counter& source, std::int32_t offset);

}  // namespace demo::api
