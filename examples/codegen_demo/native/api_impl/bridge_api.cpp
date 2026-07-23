#include "bridge_api.h"

#include <chrono>
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

}  // namespace demo::api
