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

}  // namespace demo::api
