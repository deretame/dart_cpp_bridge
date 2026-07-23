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

class BRIDGE_EXPORT Counter {
 public:
  BRIDGE_CONSTRUCTOR Counter(std::int32_t initialValue);
  BRIDGE_CONSTRUCTOR Counter();

  BRIDGE_ASYNC async_simple::coro::Lazy<std::int32_t> value() const;
  BRIDGE_SYNC std::int32_t valueSync() const;

  BRIDGE_ASYNC async_simple::coro::Lazy<void> increment(std::int32_t delta = 1);
  BRIDGE_NORMAL std::int32_t sleepAndGet(std::int32_t sleepMs);
  BRIDGE_ASYNC async_simple::coro::Lazy<std::int32_t> addList(
      const std::vector<std::int32_t>& values);
  BRIDGE_ASYNC async_simple::coro::Lazy<void> setValue(
      std::optional<std::int32_t> value);
  BRIDGE_ASYNC async_simple::coro::Lazy<Counter> duplicate() const;

  static BRIDGE_SYNC std::int32_t sum(std::int32_t a, std::int32_t b);

  BRIDGE_ASYNC async_simple::coro::Lazy<std::string> greetDartFn(
      dcb::DartFn<std::string(std::string)> callback, std::string name);

  void tickStream(dcb::StreamSink<std::int32_t> sink, std::int32_t count = 5,
                  std::int32_t intervalMs = 10);

 private:
  std::int32_t value_ = 0;
};

}  // namespace demo::api
