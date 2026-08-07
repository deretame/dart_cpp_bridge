#include "counter.h"

#include "dart_cpp_bridge/runtime.hpp"

#include <stdexec/execution.hpp>

#include <asio/post.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace demo::api {

Counter::Counter(std::int32_t initialValue) : value_(initialValue) {}

Counter::Counter() : value_(0) {}

Counter::~Counter() {
  // Custom cleanup logic goes here (close files, free resources, etc.).
  // This is called when the shared_ptr reference count reaches zero,
  // triggered by Dart dispose(), NativeFinalizer, or session close.
}

stdexec::task<std::int32_t> Counter::value() const { co_return value_; }

std::int32_t Counter::valueSync() const { return value_; }

std::string Counter::toString() const {
  return "Counter(value: " + std::to_string(value_) + ")";
}

stdexec::task<void> Counter::increment(std::int32_t delta) {
  value_ += delta;
  co_return;
}

std::int32_t Counter::sleepAndGet(std::int32_t sleepMs) {
  std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
  return value_;
}

stdexec::task<std::int32_t> Counter::addList(
    const std::vector<std::int32_t>& values) {
  for (auto v : values) {
    value_ += v;
  }
  co_return value_;
}

stdexec::task<void> Counter::setValue(
    std::optional<std::int32_t> value) {
  if (value.has_value()) {
    value_ = *value;
  }
  co_return;
}

stdexec::task<Counter> Counter::duplicate() const {
  co_return Counter(value_);
}

stdexec::task<std::int32_t> Counter::addTo(const Counter& other) {
  value_ += other.value_;
  co_return value_;
}

std::int32_t Counter::sum(std::int32_t a, std::int32_t b) { return a + b; }

stdexec::task<std::string> Counter::greetDartFn(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  auto reply = co_await callback(name);
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

stdexec::task<std::int32_t> addCounters(const Counter& a,
                                                   const Counter& b) {
  co_return a.valueSync() + b.valueSync();
}

Counter cloneWithOffset(const Counter& source, std::int32_t offset) {
  return Counter(source.valueSync() + offset);
}

}  // namespace demo::api
