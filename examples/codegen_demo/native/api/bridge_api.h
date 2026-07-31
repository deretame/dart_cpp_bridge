#pragma once

#include "dart_cpp_bridge/annotate.h"
#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <array>
#include <async_simple/coro/Lazy.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace demo::api {

// unmarked — must not appear in IR
inline std::int32_t internal_helper() { return -1; }

// data classes
struct BRIDGE_DATA_CLASS Point {
    double x;
    double y;
    std::optional<std::string> label;
};

struct BRIDGE_DATA_CLASS Rect {
    Point top_left;
    Point bottom_right;
};

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
enum class BRIDGE_EXPORT OrderStatus : std::int32_t {
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

// async → Dart: Future<List<bool>> echoBoolList(List<bool> values)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::vector<bool>> echo_bool_list(
    std::vector<bool> values);

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

// async → Dart: Future<String> greetDartFn(String Function(String) callback, String name)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> greet_dart_fn(
    dcb::DartFn<std::string(std::string)> callback, std::string name);

// normal (pool) + syncAwait → Dart: Future<String> concatDartFn(String Function(String, String) callback, ...)
BRIDGE_NORMAL
std::string concat_dart_fn(dcb::DartFn<std::string(std::string, std::string)> callback,
                           std::string a, std::string b);

// EXPERIMENT: sync + syncAwait on pool thread — measures blocking duration in µs
BRIDGE_SYNC
std::int64_t sync_dart_fn_blocking_us(dcb::DartFn<std::string(std::string)> callback,
                                      std::string input);

// FRB-style pattern: register is sync (just stores), invoke is async (pool thread calls Dart)
BRIDGE_SYNC
BRIDGE_PERSIST
bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback);

BRIDGE_NORMAL
std::string invoke_registered(std::string input);

// FRB-style pattern variant: invoke via coroutine (co_await fn(...) on io thread)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> invoke_registered_async(std::string input);

// async → Dart: Future<(int, String)> pairEcho((int, String) value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::pair<std::int32_t, std::string>> pair_echo(
    std::pair<std::int32_t, std::string> value);

// async → Dart: Future<(int, String, bool)> tupleEcho((int, String, bool) value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::tuple<std::int32_t, std::string, bool>> tuple_echo(
    std::tuple<std::int32_t, std::string, bool> value);

// stream → Dart: Stream<int> tickStream({int count = 5, int intervalMs = 10})
void tick_stream(dcb::StreamSink<std::int32_t> sink, std::int32_t count = 5,
                 std::int32_t interval_ms = 10);

// async + optional sink → Dart: Future<String> downloadWithProgress({required String url, StreamController<int>? progress})
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress);

// data class tests
// async → Dart: Future<double> distance(Point a, Point b)
BRIDGE_ASYNC
async_simple::coro::Lazy<double> distance(Point a, Point b);

// async → Dart: Future<Point> scale(Point p, double factor)
BRIDGE_ASYNC
async_simple::coro::Lazy<Point> scale(Point p, double factor);

// async → Dart: Future<Rect> boundingBox(List<Point> points)
BRIDGE_ASYNC
async_simple::coro::Lazy<Rect> bounding_box(std::vector<Point> points);

// --- Runtime error propagation tests (R01-R05) ---

// R01: async throw → Dart Future receives StateError
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> fail_async(std::string msg);

// R02: sync throw → Dart throws StateError
BRIDGE_SYNC
std::int32_t fail_sync(std::string msg);

// R03: normal throw → Dart Future receives StateError
BRIDGE_NORMAL
std::int32_t fail_normal(std::string msg);

// R04: non-std exception → Dart receives "unknown"
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> fail_non_std();

// R05: stream emits data then errors
void fail_stream(dcb::StreamSink<std::int32_t> sink, std::string msg);

// --- Deep nesting test (G03) ---

// G03: 3-level nested vector → Dart List<List<List<int>>>
BRIDGE_SYNC
std::vector<std::vector<std::vector<std::int32_t>>> nested_cube(std::int32_t n);

// --- Time (std::chrono::system_clock::time_point ↔ Dart DateTime) ---
// Wire format: i64 microseconds since Unix epoch (UTC, no offset).

// async → Dart: Future<DateTime> echoTime(DateTime value)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::chrono::system_clock::time_point> echo_time(
    std::chrono::system_clock::time_point value);

// sync → Dart: DateTime echoTimeSync(DateTime value)
BRIDGE_SYNC
std::chrono::system_clock::time_point echo_time_sync(
    std::chrono::system_clock::time_point value);

}  // namespace demo::api
