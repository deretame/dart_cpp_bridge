#include "counter.h"

#include "dart_cpp_bridge/runtime.hpp"

#include <asio/post.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace demo::api {

Counter::Counter(std::int32_t initialValue) : value_(initialValue) {}
Counter::Counter() : value_(0) {}

async_simple::coro::Lazy<std::int32_t> Counter::value() const { co_return value_; }

std::int32_t Counter::valueSync() const { return value_; }

async_simple::coro::Lazy<void> Counter::increment(std::int32_t delta) {
  value_ += delta;
  co_return;
}

std::int32_t Counter::sleepAndGet(std::int32_t sleepMs) {
  std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
  return value_;
}

async_simple::coro::Lazy<std::int32_t> Counter::addList(
    const std::vector<std::int32_t>& values) {
  for (auto v : values) {
    value_ += v;
  }
  co_return value_;
}

async_simple::coro::Lazy<void> Counter::setValue(
    std::optional<std::int32_t> value) {
  if (value.has_value()) {
    value_ = *value;
  }
  co_return;
}

async_simple::coro::Lazy<Counter> Counter::duplicate() const {
  co_return Counter(value_);
}

std::int32_t Counter::sum(std::int32_t a, std::int32_t b) { return a + b; }

async_simple::coro::Lazy<std::string> Counter::greetDartFn(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  auto reply = co_await callback.callAsync(name);
  co_return std::string("hello, ") + reply;
}

void Counter::tickStream(dcb::StreamSink<std::int32_t> sink, std::int32_t count,
                         std::int32_t intervalMs) {
  const auto current = value_;
  asio::post(dcb::Runtime::instance().pool(),
             [sink = std::move(sink), count, intervalMs, current]() mutable {
               for (std::int32_t i = 0; i < count; ++i) {
                 sink.add(current);
                 if (intervalMs > 0) {
                   std::this_thread::sleep_for(
                       std::chrono::milliseconds(intervalMs));
                 }
               }
               sink.end();
             });
}

}  // namespace demo::api
