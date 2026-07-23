#pragma once

#include "dart_cpp_bridge/annotate.h"
#include "dart_cpp_bridge/codec.hpp"

#include <array>
#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

// async → Dart: Future<List<int>> echoList(List<int> values)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::vector<std::int32_t>> echo_list(
    std::vector<std::int32_t> values);

// async → Dart: Future<int> sumArray(List<int> values)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> sum_array(
    std::array<std::int32_t, 4> values);

// async → Dart: Future<int> sumScores(Map<String, int> scores)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> sum_scores(
    std::unordered_map<std::string, std::int32_t> scores);

// async → Dart: Future<int> sumSet(Set<int> values)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> sum_set(
    std::unordered_set<std::int32_t> values);

// async → Dart: Future<BigInt> echoI128(BigInt value)
BRIDGE_ASYNC
async_simple::coro::Lazy<dcb::Int128> echo_i128(dcb::Int128 value);

// async → Dart: Future<BigInt> echoU128(BigInt value)
BRIDGE_ASYNC
async_simple::coro::Lazy<dcb::UInt128> echo_u128(dcb::UInt128 value);

}  // namespace demo::api
