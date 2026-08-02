#include "bridge_api.h"

#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <async_simple/Signal.h>

#include <asio/post.hpp>
#include <async_simple/coro/Collect.h>
#include <async_simple/coro/Sleep.h>
#include <async_simple/coro/SyncAwait.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>

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

async_simple::coro::Lazy<std::vector<bool>> echo_bool_list(
    std::vector<bool> values) {
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
  auto reply = co_await callback(name);
  co_return std::string("hello, ") + reply;
}

std::string concat_dart_fn(
    dcb::DartFn<std::string(std::string, std::string)> callback,
    std::string a, std::string b) {
  auto reply = async_simple::coro::syncAwait(dcb::spawn(callback(a, b)));
  return "sync:" + reply;
}

std::int64_t sync_dart_fn_blocking_us(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto t0 = std::chrono::steady_clock::now();
  auto reply = async_simple::coro::syncAwait(dcb::spawn(callback(input)));
  auto t1 = std::chrono::steady_clock::now();
  (void)reply;
  return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
}

// --- FRB-style register/invoke pattern ---
namespace {
dcb::DartFn<std::string(std::string)> g_registered_fn;
}  // namespace

bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback) {
  g_registered_fn = std::move(callback);
  return static_cast<bool>(g_registered_fn);
}

std::string invoke_registered(std::string input) {
  if (!g_registered_fn) {
    throw std::runtime_error("no registered dart fn");
  }
  auto reply = async_simple::coro::syncAwait(dcb::spawn(g_registered_fn(input)));
  return "registered:" + reply;
}

async_simple::coro::Lazy<std::string> invoke_registered_async(std::string input) {
  if (!g_registered_fn) {
    throw std::runtime_error("no registered dart fn");
  }
  auto reply = co_await g_registered_fn(input);
  co_return "async_registered:" + reply;
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

async_simple::coro::Lazy<std::string> download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  // Simulate a download with 5 progress steps. Events are emitted as part of
  // the asynchronous work (between co_await points), never by blocking io.
  for (std::int32_t i = 1; i <= 5; ++i) {
    if (progress) {
      progress->add(i * 20);  // 20, 40, 60, 80, 100
      co_await async_simple::coro::sleep(std::chrono::milliseconds(10));
    }
  }
  co_return std::string("downloaded: ") + url;
}

namespace {
// Free coroutine function: parameters are copied into the coroutine frame, so
// passing a moved-in StreamSink is safe (a coroutine lambda would instead
// reference captures of the temporary lambda object, which dangles).
async_simple::coro::Lazy<> emit_sync_progress(dcb::StreamSink<std::int32_t> sink) {
  for (std::int32_t i = 1; i <= 5; ++i) {
    sink.add(i * 20);  // 20, 40, 60, 80, 100
  }
  co_return;
}
}  // namespace

std::string sync_download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  // Sync variant: do NOT emit inside the blocking FFI call. Spawn a coroutine
  // that sends the events asynchronously; Dart receives them from the reply
  // port queue right after the sync call returns.
  if (progress) {
    dcb::spawn_detached(emit_sync_progress(std::move(*progress)));
  }
  return std::string("downloaded: ") + url;
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
  r.label = p.label;
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

// --- Runtime error propagation tests ---

async_simple::coro::Lazy<std::int32_t> fail_async(std::string msg) {
  throw std::runtime_error(msg.empty() ? "fail_async" : msg);
  co_return 0;  // unreachable
}

std::int32_t fail_sync(std::string msg) {
  throw std::runtime_error(msg.empty() ? "fail_sync" : msg);
}

std::int32_t fail_normal(std::string msg) {
  throw std::runtime_error(msg.empty() ? "fail_normal" : msg);
}

async_simple::coro::Lazy<std::int32_t> fail_non_std() {
  throw 42;  // non-std::exception
  co_return 0;  // unreachable
}

void fail_stream(dcb::StreamSink<std::int32_t> sink, std::string msg) {
  asio::post(dcb::Runtime::instance().pool(),
             [sink = std::move(sink), msg = std::move(msg)]() mutable {
               sink.add(1);
               sink.add(2);
               sink.error(msg.empty() ? "fail_stream" : msg);
             });
}

// --- Deep nesting test ---

std::vector<std::vector<std::vector<std::int32_t>>> nested_cube(std::int32_t n) {
  std::vector<std::vector<std::vector<std::int32_t>>> cube;
  for (std::int32_t i = 0; i < n; ++i) {
    std::vector<std::vector<std::int32_t>> plane;
    for (std::int32_t j = 0; j < n; ++j) {
      std::vector<std::int32_t> row;
      for (std::int32_t k = 0; k < n; ++k) {
        row.push_back(i * 100 + j * 10 + k);
      }
      plane.push_back(std::move(row));
    }
    cube.push_back(std::move(plane));
  }
  return cube;
}

// --- Time (std::chrono::system_clock::time_point ↔ Dart DateTime) ---

async_simple::coro::Lazy<std::chrono::system_clock::time_point> echo_time(
    std::chrono::system_clock::time_point value) {
  co_return value;
}

std::chrono::system_clock::time_point echo_time_sync(
    std::chrono::system_clock::time_point value) {
  return value;
}

// --- async-simple Signal/Slot cancellation ---

namespace {

// Global task registry: task_id → cancellation signal.
//
// cancel_task() is called from the Dart caller thread while the coroutine
// runs on the io thread, so the map is mutex-protected. Signal::emits() is
// thread-safe; the Slot itself is only touched by the coroutine that owns it.
std::mutex g_task_signal_mutex;
std::unordered_map<std::string, std::shared_ptr<async_simple::Signal>>
    g_task_signals;

// Removes the map entry when the coroutine exits, whether it completed
// normally, was cancelled, or failed.
struct TaskSignalGuard {
  std::string task_id;
  ~TaskSignalGuard() {
    std::lock_guard<std::mutex> lock(g_task_signal_mutex);
    g_task_signals.erase(task_id);
  }
};

}  // namespace

// The task body. The signal is bound to this coroutine chain via
// Lazy::setLazyLocal in cancellable_task(), so co_await
// async_simple::coro::sleep(...) inherits the Slot and is interrupted by
// SignalType::Terminate (the library's AsioExecutor cancels the underlying
// asio timer). SignalException is rethrown with a stable demo message.
async_simple::coro::Lazy<std::string> cancellable_task_impl(
    std::string task_id, std::int32_t steps, std::int32_t interval_ms) {
  try {
    for (std::int32_t i = 0; i < steps; ++i) {
      co_await async_simple::coro::sleep(
          std::chrono::milliseconds(interval_ms));
    }
  } catch (const async_simple::SignalException&) {
    throw async_simple::SignalException(
        async_simple::SignalType::Terminate,
        "task cancelled by signal: " + task_id);
  }
  co_return std::string("done:") + task_id;
}

async_simple::coro::Lazy<std::string> cancellable_task(
    std::string task_id, std::int32_t steps, std::int32_t interval_ms) {
  auto signal = async_simple::Signal::create();
  {
    std::lock_guard<std::mutex> lock(g_task_signal_mutex);
    g_task_signals[task_id] = signal;
  }
  TaskSignalGuard guard{task_id};
  co_return co_await std::move(
      cancellable_task_impl(task_id, steps, interval_ms))
      .setLazyLocal(signal.get());
}

bool cancel_task(std::string task_id) {
  std::shared_ptr<async_simple::Signal> signal;
  {
    std::lock_guard<std::mutex> lock(g_task_signal_mutex);
    auto it = g_task_signals.find(task_id);
    if (it == g_task_signals.end()) {
      return false;
    }
    signal = it->second;
  }
  return signal->emits(async_simple::SignalType::Terminate) !=
         async_simple::SignalType::None;
}

bool is_task_running(std::string task_id) {
  std::lock_guard<std::mutex> lock(g_task_signal_mutex);
  return g_task_signals.find(task_id) != g_task_signals.end();
}

// --- async-simple collectAll / collectAny ---

namespace {

// Set when a collect* sub-task observes the Terminate signal and unwinds
// with SignalException. Used to verify that collectAny<Terminate> /
// collectAll<Terminate> really forwards the cancellation signal.
std::atomic<int> g_collect_cancel_observed{0};

async_simple::coro::Lazy<std::int32_t> collect_int_task(std::int32_t v) {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(10));
  co_return v;
}

async_simple::coro::Lazy<std::string> collect_str_task(std::string s) {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(10));
  co_return s;
}

async_simple::coro::Lazy<std::string> collect_fail_task() {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(5));
  throw std::runtime_error("boom in collectAll");
  co_return "unreachable";
}

// A sub-task that cooperates with the collect* cancellation signal: the
// collect* awaiter binds a Slot to this Lazy chain, so plain
// async_simple::coro::sleep() is interrupted by the forwarded
// SignalType::Terminate and throws SignalException (first task finished).
async_simple::coro::Lazy<std::string> collect_cancellable_slow_task() {
  try {
    for (std::int32_t i = 0; i < 1000; ++i) {
      co_await async_simple::coro::sleep(std::chrono::milliseconds(20));
    }
    co_return "slow-finished";
  } catch (const async_simple::SignalException&) {
    g_collect_cancel_observed.fetch_add(1);
    throw;
  }
}

async_simple::coro::Lazy<std::string> collect_fast_str_task() {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(5));
  co_return "ok";
}

async_simple::coro::Lazy<std::int32_t> collect_any_fast_int_task() {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(5));
  co_return 42;
}

async_simple::coro::Lazy<std::string> collect_any_slow_str_task() {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(200));
  co_return "slow";
}

}  // namespace

async_simple::coro::Lazy<std::string> collect_all_demo() {
  auto res = co_await async_simple::coro::collectAll(
      collect_int_task(1), collect_str_task("two"), collect_int_task(3));
  const auto& a = std::get<0>(res);
  const auto& b = std::get<1>(res);
  const auto& c = std::get<2>(res);
  co_return (a.hasError() ? std::string("err") : std::to_string(a.value())) +
           "|" + (b.hasError() ? std::string("err") : b.value()) + "|" +
           (c.hasError() ? std::string("err") : std::to_string(c.value()));
}

async_simple::coro::Lazy<std::int32_t> collect_all_para_demo() {
  std::vector<async_simple::coro::Lazy<std::int32_t>> input;
  for (std::int32_t i = 0; i < 4; ++i) {
    input.push_back(collect_int_task(i));
  }
  auto res = co_await async_simple::coro::collectAllPara(std::move(input));
  std::int32_t sum = 0;
  for (const auto& t : res) {
    sum += t.value();
  }
  co_return sum;
}

async_simple::coro::Lazy<std::string> collect_all_error_demo() {
  auto res = co_await async_simple::coro::collectAll(
      collect_str_task("hello"), collect_fail_task());
  const auto& ok = std::get<0>(res);
  const auto& bad = std::get<1>(res);
  co_return (ok.hasError() ? std::string("ok-error") : ok.value()) + "|" +
           (bad.hasError() ? std::string("err-captured")
                           : std::string("err-missed"));
}

async_simple::coro::Lazy<std::string> collect_all_cancel_demo() {
  g_collect_cancel_observed.store(0);
  auto res = co_await async_simple::coro::collectAll<
      async_simple::SignalType::Terminate>(collect_fast_str_task(),
                                           collect_cancellable_slow_task());
  const auto& fast = std::get<0>(res);
  const auto& slow = std::get<1>(res);
  co_return (fast.hasError() ? std::string("fast-error") : fast.value()) + "|" +
           (slow.hasError() ? std::string("slow-cancelled")
                            : std::string("slow-finished"));
}

async_simple::coro::Lazy<std::string> collect_any_demo() {
  auto res = co_await async_simple::coro::collectAny(
      collect_any_slow_str_task(), collect_any_fast_int_task());
  if (res.index() == 0) {
    co_return "winner=slow,value=" + std::get<0>(res).value();
  }
  co_return "winner=fast,value=" + std::to_string(std::get<1>(res).value());
}

async_simple::coro::Lazy<std::string> collect_any_cancel_demo() {
  g_collect_cancel_observed.store(0);
  auto res = co_await async_simple::coro::collectAny<
      async_simple::SignalType::Terminate>(collect_cancellable_slow_task(),
                                           collect_any_fast_int_task());
  // Let the cancelled loser observe the signal and unwind before checking.
  co_await async_simple::coro::sleep(std::chrono::milliseconds(30));
  const bool loser_cancelled = g_collect_cancel_observed.load() > 0;
  const std::string winner =
      res.index() == 1
          ? "winner=fast,value=" + std::to_string(std::get<1>(res).value())
          : "winner=slow";
  co_return winner + "|" + (loser_cancelled ? "loser-cancelled"
                                            : "loser-still-running");
}

}  // namespace demo::api
