#pragma once

#include "dart_cpp_bridge/annotate.h"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <optional>
#include <string>

namespace demo::api {

// unmarked — must not appear in IR
inline std::int32_t internal_helper() { return -1; }

// sync → Dart: int bridgeVersion()
BRIDGE_SYNC
std::int32_t bridge_version();

// async → Dart: Future<int> add(int a, int b)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);

// normal (pool) → Dart: Future<String> sleepGreeting(String name)
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

// enum test
enum class OrderStatus : std::int32_t {
  kCreated = 0,
  kPaid = 1,
  kShipped = 2,
};

// async → Dart: Future<OrderStatus> nextStatus(OrderStatus current)
BRIDGE_ASYNC
async_simple::coro::Lazy<OrderStatus> next_status(OrderStatus current);

// async → Dart: Future<int?> maybeDouble(int? value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::optional<std::int32_t>> maybe_double(
    std::optional<std::int32_t> value);

// async → Dart: Future<int> incrementU32(int value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::uint32_t> increment_u32(std::uint32_t value);

// async → Dart: Future<int> incrementI64(int value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int64_t> increment_i64(std::int64_t value);

// async → Dart: Future<bool> negateBool(bool value)
BRIDGE_ASYNC
async_simple::coro::Lazy<bool> negate_bool(bool value);

// async → Dart: Future<String?> optionalString(String? value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::optional<std::string>> optional_string(
    std::optional<std::string> value);

// async → Dart: Future<OrderStatus?> optionalStatus(OrderStatus? value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::optional<OrderStatus>> optional_status(
    std::optional<OrderStatus> value);

}  // namespace demo::api
