#include "bridge_api.h"

#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <asio/post.hpp>

#include <chrono>
#include <cmath>
#include <thread>

namespace demo::api {

std::int32_t bridge_version() { return 42; }

async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b) {
  co_return a + b;
}

std::string sleep_greeting(std::string name) {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return std::string("hello, ") + name;
}

async_simple::coro::Lazy<OrderStatus> next_status(OrderStatus current) {
  switch (current) {
    case OrderStatus::kCreated:
      co_return OrderStatus::kPaid;
    case OrderStatus::kPaid:
      co_return OrderStatus::kShipped;
    default:
      co_return OrderStatus::kCreated;
  }
}

async_simple::coro::Lazy<std::optional<std::int32_t>> maybe_double(
    std::optional<std::int32_t> value) {
  if (value.has_value()) {
    co_return std::optional<std::int32_t>(value.value() * 2);
  }
  co_return std::nullopt;
}

async_simple::coro::Lazy<std::uint32_t> increment_u32(std::uint32_t value) {
  co_return value + 1;
}

async_simple::coro::Lazy<std::int64_t> increment_i64(std::int64_t value) {
  co_return value + 1;
}

async_simple::coro::Lazy<bool> negate_bool(bool value) { co_return !value; }

async_simple::coro::Lazy<std::optional<std::string>> optional_string(
    std::optional<std::string> value) {
  if (value.has_value()) {
    co_return std::optional<std::string>(value.value() + "!");
  }
  co_return std::nullopt;
}

async_simple::coro::Lazy<std::optional<OrderStatus>> optional_status(
    std::optional<OrderStatus> value) {
  if (!value.has_value()) {
    co_return std::nullopt;
  }
  switch (value.value()) {
    case OrderStatus::kCreated:
      co_return std::optional<OrderStatus>(OrderStatus::kPaid);
    case OrderStatus::kPaid:
      co_return std::optional<OrderStatus>(OrderStatus::kShipped);
    default:
      co_return std::optional<OrderStatus>(OrderStatus::kCreated);
  }
}

async_simple::coro::Lazy<std::vector<std::int32_t>> echo_list(
    std::vector<std::int32_t> values) {
  co_return values;
}

async_simple::coro::Lazy<std::int32_t> sum_array(
    std::array<std::int32_t, 4> values) {
  std::int32_t total = 0;
  for (auto v : values) total += v;
  co_return total;
}

async_simple::coro::Lazy<std::int32_t> sum_scores(
    std::unordered_map<std::string, std::int32_t> scores) {
  std::int32_t total = 0;
  for (const auto& [k, v] : scores) total += v;
  co_return total;
}

async_simple::coro::Lazy<std::int32_t> sum_set(
    std::unordered_set<std::int32_t> values) {
  std::int32_t total = 0;
  for (auto v : values) total += v;
  co_return total;
}

async_simple::coro::Lazy<dcb::Int128> echo_i128(dcb::Int128 value) {
  co_return value;
}

async_simple::coro::Lazy<dcb::UInt128> echo_u128(dcb::UInt128 value) {
  co_return value;
}

async_simple::coro::Lazy<std::string> greet_dart_fn(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  auto reply = co_await callback.callAsync(name);
  co_return std::string("hello, ") + reply;
}

async_simple::coro::Lazy<std::pair<std::int32_t, std::string>> pair_echo(
    std::pair<std::int32_t, std::string> value) {
  co_return value;
}

async_simple::coro::Lazy<std::tuple<std::int32_t, std::string, bool>> tuple_echo(
    std::tuple<std::int32_t, std::string, bool> value) {
  co_return value;
}

void tick_stream(dcb::StreamSink<std::int32_t> sink, std::int32_t count,
                 std::int32_t interval_ms) {
  asio::post(dcb::Runtime::instance().pool(),
             [sink = std::move(sink), count, interval_ms]() mutable {
               for (std::int32_t i = 0; i < count; ++i) {
                 sink.add(i);
                 if (interval_ms > 0) {
                   std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                 }
               }
               sink.end();
             });
}

async_simple::coro::Lazy<double> distance(Point a, Point b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  co_return std::sqrt(dx * dx + dy * dy);
}

async_simple::coro::Lazy<Point> scale(Point p, double factor) {
  Point r;
  r.x = p.x * factor;
  r.y = p.y * factor;
  co_return r;
}

async_simple::coro::Lazy<Rect> bounding_box(std::vector<Point> points) {
  Rect r;
  if (points.empty()) {
    r.top_left = {0.0, 0.0};
    r.bottom_right = {0.0, 0.0};
    co_return r;
  }
  r.top_left = points[0];
  r.bottom_right = points[0];
  for (const auto& p : points) {
    r.top_left.x = std::min(r.top_left.x, p.x);
    r.top_left.y = std::min(r.top_left.y, p.y);
    r.bottom_right.x = std::max(r.bottom_right.x, p.x);
    r.bottom_right.y = std::max(r.bottom_right.y, p.y);
  }
  co_return r;
}

}  // namespace demo::api
