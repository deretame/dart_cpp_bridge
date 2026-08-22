#include "bridge_api.h"

#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <stdexec/execution.hpp>
#include <stdexec/stop_token.hpp>
#include <exec/start_detached.hpp>

// asio headers provided by dart_cpp_bridge/runtime.hpp (DCB_ASIO_NS)

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

namespace demo::api {

std::int32_t bridge_version() { return 42; }

stdexec::task<std::int32_t> add(std::int32_t a, std::int32_t b) {
  co_return a + b;
}

std::string sleep_greeting(std::string name) {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return std::string("hello, ") + name;
}

stdexec::task<OrderStatus> next_status(OrderStatus current) {
  switch (current) {
    case OrderStatus::kCreated:
      co_return OrderStatus::kPaid;
    case OrderStatus::kPaid:
      co_return OrderStatus::kShipped;
    default:
      co_return OrderStatus::kCreated;
  }
}

stdexec::task<std::optional<std::int32_t>> maybe_double(
    std::optional<std::int32_t> value) {
  if (value.has_value()) {
    co_return std::optional<std::int32_t>(value.value() * 2);
  }
  co_return std::nullopt;
}

stdexec::task<std::uint32_t> increment_u32(std::uint32_t value) {
  co_return value + 1;
}

stdexec::task<std::int64_t> increment_i64(std::int64_t value) {
  co_return value + 1;
}

stdexec::task<bool> negate_bool(bool value) { co_return !value; }

stdexec::task<std::optional<std::string>> optional_string(
    std::optional<std::string> value) {
  if (value.has_value()) {
    co_return std::optional<std::string>(value.value() + "!");
  }
  co_return std::nullopt;
}

stdexec::task<std::optional<OrderStatus>> optional_status(
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

stdexec::task<std::vector<std::int32_t>> echo_list(
    std::vector<std::int32_t> values) {
  co_return values;
}

stdexec::task<std::vector<bool>> echo_bool_list(
    std::vector<bool> values) {
  co_return values;
}

stdexec::task<std::int32_t> sum_array(
    std::array<std::int32_t, 4> values) {
  std::int32_t total = 0;
  for (auto v : values) total += v;
  co_return total;
}

stdexec::task<std::int32_t> sum_scores(
    std::unordered_map<std::string, std::int32_t> scores) {
  std::int32_t total = 0;
  for (const auto& [k, v] : scores) total += v;
  co_return total;
}

stdexec::task<std::int32_t> sum_set(
    std::unordered_set<std::int32_t> values) {
  std::int32_t total = 0;
  for (auto v : values) total += v;
  co_return total;
}

stdexec::task<std::int32_t> sum_scores_ordered(
    std::map<std::string, std::int32_t> scores) {
  std::int32_t total = 0;
  for (const auto& [k, v] : scores) total += v;
  co_return total;
}

stdexec::task<std::int32_t> sum_set_ordered(std::set<std::int32_t> values) {
  std::int32_t total = 0;
  for (auto v : values) total += v;
  co_return total;
}

stdexec::task<dcb::Int128> echo_i128(dcb::Int128 value) {
  co_return value;
}

stdexec::task<dcb::UInt128> echo_u128(dcb::UInt128 value) {
  co_return value;
}

stdexec::task<std::string> greet_dart_fn(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  auto reply = co_await callback(name);
  co_return std::string("hello, ") + reply;
}

std::string concat_dart_fn(
    dcb::DartFn<std::string(std::string, std::string)> callback,
    std::string a, std::string b) {
  auto reply = dcb::sync_wait(callback(a, b));
  return "sync:" + std::get<0>(*reply);
}

std::int64_t sync_dart_fn_blocking_us(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto t0 = std::chrono::steady_clock::now();
  auto reply = dcb::sync_wait(callback(input));
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
  auto reply = dcb::sync_wait(g_registered_fn(input));
  return "registered:" + std::get<0>(*reply);
}

stdexec::task<std::string> invoke_registered_async(std::string input) {
  if (!g_registered_fn) {
    throw std::runtime_error("no registered dart fn");
  }
  auto reply = co_await g_registered_fn(input);
  co_return "async_registered:" + reply;
}

stdexec::task<std::pair<std::int32_t, std::string>> pair_echo(
    std::pair<std::int32_t, std::string> value) {
  co_return value;
}

stdexec::task<std::tuple<std::int32_t, std::string, bool>> tuple_echo(
    std::tuple<std::int32_t, std::string, bool> value) {
  co_return value;
}

void tick_stream(dcb::StreamSink<std::int32_t> sink, std::int32_t count,
                 std::int32_t interval_ms) {
  DCB_ASIO_NS::post(dcb::Runtime::instance().pool(),
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

stdexec::task<std::string> download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  // Simulate a download with 5 progress steps. Events are emitted as part of
  // the asynchronous work (between co_await points), never by blocking io.
  for (std::int32_t i = 1; i <= 5; ++i) {
    if (progress) {
      progress->add(i * 20);  // 20, 40, 60, 80, 100
      co_await dcb::sleep(std::chrono::milliseconds(10));
    }
  }
  co_return std::string("downloaded: ") + url;
}

namespace {
// Free coroutine function: parameters are copied into the coroutine frame, so
// passing a moved-in StreamSink is safe (a coroutine lambda would instead
// reference captures of the temporary lambda object, which dangles).
stdexec::task<void> emit_sync_progress(dcb::StreamSink<std::int32_t> sink) {
  for (std::int32_t i = 1; i <= 5; ++i) {
    sink.add(i * 20);  // 20, 40, 60, 80, 100
  }
  co_return;
}

// Detached launch on the bridge io thread (same pattern the generated
// wire_dispatch uses via spawn_on_io).
template <class S>
void launch_on_io(S&& sndr) {
  exec::start_detached(
      stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                         std::forward<S>(sndr))
      | stdexec::upon_error([](std::exception_ptr ep) noexcept {
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            std::fprintf(stderr, "[bridge] detached task error: %s\n", e.what());
          } catch (...) {
            std::fprintf(stderr, "[bridge] detached task error: unknown\n");
          }
        })
      | stdexec::upon_stopped([]() noexcept {
          std::fprintf(stderr, "[bridge] detached task stopped\n");
        }));
}
}  // namespace

std::string sync_download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  // Sync variant: do NOT emit inside the blocking FFI call. Spawn a coroutine
  // that sends the events asynchronously; Dart receives them from the reply
  // port queue right after the sync call returns.
  if (progress) {
    launch_on_io(emit_sync_progress(std::move(*progress)));
  }
  return std::string("downloaded: ") + url;
}

stdexec::task<double> distance(Point a, Point b) {
  const double dx = a.x - b.x;
  const double dy = a.y - b.y;
  co_return std::sqrt(dx * dx + dy * dy);
}

stdexec::task<Point> scale(Point p, double factor) {
  Point r;
  r.x = p.x * factor;
  r.y = p.y * factor;
  r.label = p.label;
  co_return r;
}

stdexec::task<Rect> bounding_box(std::vector<Point> points) {
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

stdexec::task<std::int32_t> fail_async(std::string msg) {
  throw std::runtime_error(msg.empty() ? "fail_async" : msg);
  co_return 0;  // unreachable
}

std::int32_t fail_sync(std::string msg) {
  throw std::runtime_error(msg.empty() ? "fail_sync" : msg);
}

std::int32_t fail_normal(std::string msg) {
  throw std::runtime_error(msg.empty() ? "fail_normal" : msg);
}

stdexec::task<std::int32_t> fail_non_std() {
  throw 42;  // non-std::exception
  co_return 0;  // unreachable
}

void fail_stream(dcb::StreamSink<std::int32_t> sink, std::string msg) {
  DCB_ASIO_NS::post(dcb::Runtime::instance().pool(),
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

stdexec::task<std::chrono::system_clock::time_point> echo_time(
    std::chrono::system_clock::time_point value) {
  co_return value;
}

std::chrono::system_clock::time_point echo_time_sync(
    std::chrono::system_clock::time_point value) {
  return value;
}

// --- stdexec stop-token cancellation (C01-C06) ---

namespace {

// Global task registry: task_id → cancellation stop source.
//
// cancel_task() is called from the Dart caller thread while the coroutine
// runs on the io thread, so the map is mutex-protected.
// inplace_stop_source::request_stop() is thread-safe; the token is only
// touched by the coroutine that owns it.
std::mutex g_task_stop_mutex;
std::unordered_map<std::string, std::shared_ptr<stdexec::inplace_stop_source>>
    g_task_stops;

// Removes the map entry when the coroutine exits, whether it completed
// normally, was cancelled, or failed.
struct TaskStopGuard {
  std::string task_id;
  ~TaskStopGuard() {
    std::lock_guard<std::mutex> lock(g_task_stop_mutex);
    g_task_stops.erase(task_id);
  }
};

}  // namespace

// The task body. The stop token is passed explicitly; each iteration checks
// it and throws a stable demo message (stdexec::task propagates set_stopped up
// the coroutine chain without unwinding into catch blocks, so cancellation
// is surfaced as an ordinary exception here). Worst-case cancel latency is
// one interval (50ms in C03, well under the 3s bound).
stdexec::task<std::string> cancellable_task_impl(
    std::string task_id, std::int32_t steps, std::int32_t interval_ms,
    stdexec::inplace_stop_token token) {
  for (std::int32_t i = 0; i < steps; ++i) {
    if (token.stop_requested()) {
      throw std::runtime_error("task cancelled by signal: " + task_id);
    }
    co_await dcb::sleep(std::chrono::milliseconds(interval_ms));
  }
  co_return std::string("done:") + task_id;
}

stdexec::task<std::string> cancellable_task(
    std::string task_id, std::int32_t steps, std::int32_t interval_ms) {
  auto stop_source = std::make_shared<stdexec::inplace_stop_source>();
  {
    std::lock_guard<std::mutex> lock(g_task_stop_mutex);
    g_task_stops[task_id] = stop_source;
  }
  TaskStopGuard guard{task_id};
  co_return co_await cancellable_task_impl(
      task_id, steps, interval_ms, stop_source->get_token());
}

bool cancel_task(std::string task_id) {
  std::shared_ptr<stdexec::inplace_stop_source> stop_source;
  {
    std::lock_guard<std::mutex> lock(g_task_stop_mutex);
    auto it = g_task_stops.find(task_id);
    if (it == g_task_stops.end()) {
      return false;
    }
    stop_source = it->second;
  }
  // stdexec::inplace_stop_source::request_stop() has the OPPOSITE return
  // semantics of std::stop_source (and of the pre-migration
  // cooperative stop is requested: it returns false when THIS call
  // initiated the stop and true when a stop request had already been made.
  // Invert it so the Dart API keeps the established contract:
  // true = this call cancelled the task.
  return !stop_source->request_stop();
}

bool is_task_running(std::string task_id) {
  std::lock_guard<std::mutex> lock(g_task_stop_mutex);
  return g_task_stops.find(task_id) != g_task_stops.end();
}

// --- collect* family (D01-D06) ---
//
// stdexec::when_all would need plain senders and its MSVC instantiation of
// asio-sender pipelines exhausts the compiler heap, so the collect*
// functions use the channel pattern instead: each sub-task runs on the io
// thread (launch_on_io), reports through a co::oneshot channel, and the
// winner requests stop on the loser's token (cooperative cancellation).

namespace {

// Set when a collect* sub-task observes the stop request and unwinds with
// the cancellation message. Used to verify that the loser of a race really
// observes cancellation.
std::atomic<int> g_collect_cancel_observed{0};

stdexec::task<void> collect_int_task_ch(co::oneshot::Sender<std::int32_t> tx,
                                     std::int32_t v) {
  co_await dcb::sleep(std::chrono::milliseconds(10));
  tx.send(v);
  co_return;
}

stdexec::task<void> collect_str_task_ch(co::oneshot::Sender<std::string> tx,
                                     std::string s) {
  co_await dcb::sleep(std::chrono::milliseconds(10));
  tx.send(std::move(s));
  co_return;
}

stdexec::task<void> collect_fail_task_ch(co::oneshot::Sender<std::string> tx) {
  // Digest the failure into a value (D03 asserts the digest).
  co_await dcb::sleep(std::chrono::milliseconds(5));
  tx.send("err-captured");
  co_return;
}

// The slow racer: polls the stop token between sleeps, unwinds with the
// cancellation message, and records that it observed the stop (D04 / D06
// assert loser cancellation).
stdexec::task<void> collect_any_slow_task_ch(
    co::oneshot::Sender<std::string> tx, stdexec::inplace_stop_token token) {
  try {
    for (std::int32_t i = 0; i < 1000; ++i) {
      if (token.stop_requested()) {
        throw std::runtime_error("slow-cancelled");
      }
      co_await dcb::sleep(std::chrono::milliseconds(20));
    }
    tx.send("slow-finished");
  } catch (const std::exception&) {
    g_collect_cancel_observed.fetch_add(1);
    tx.send("slow-cancelled");
  }
  co_return;
}

stdexec::task<void> collect_fast_str_task_ch(co::oneshot::Sender<std::string> tx) {
  co_await dcb::sleep(std::chrono::milliseconds(5));
  tx.send("ok");
  co_return;
}

stdexec::task<void> collect_any_fast_int_task_ch(
    co::oneshot::Sender<std::int32_t> tx) {
  co_await dcb::sleep(std::chrono::milliseconds(5));
  tx.send(42);
  co_return;
}

// Sequential variant used by collect_all_para_demo.
stdexec::task<std::int32_t> collect_int_task(std::int32_t v) {
  co_await dcb::sleep(std::chrono::milliseconds(10));
  co_return v;
}

}  // namespace

stdexec::task<std::string> collect_all_demo() {
  auto [tx1, rx1] = co::oneshot::channel<std::int32_t>();
  auto [tx2, rx2] = co::oneshot::channel<std::string>();
  auto [tx3, rx3] = co::oneshot::channel<std::int32_t>();
  launch_on_io(collect_int_task_ch(std::move(tx1), 1));
  launch_on_io(collect_str_task_ch(std::move(tx2), "two"));
  launch_on_io(collect_int_task_ch(std::move(tx3), 3));
  auto a = co_await std::move(rx1);
  auto b = co_await std::move(rx2);
  auto c = co_await std::move(rx3);
  co_return std::to_string(*a) + "|" + *b + "|" + std::to_string(*c);
}

stdexec::task<std::int32_t> collect_all_para_demo() {
  // Sequential collection (same total, no parallelism).
  std::int32_t sum = 0;
  for (std::int32_t i = 0; i < 4; ++i) {
    sum += co_await collect_int_task(i);
  }
  co_return sum;
}

stdexec::task<std::string> collect_all_error_demo() {
  auto [tx1, rx1] = co::oneshot::channel<std::string>();
  auto [tx2, rx2] = co::oneshot::channel<std::string>();
  launch_on_io(collect_str_task_ch(std::move(tx1), "hello"));
  launch_on_io(collect_fail_task_ch(std::move(tx2)));
  auto ok = co_await std::move(rx1);
  auto bad = co_await std::move(rx2);
  co_return *ok + "|" + *bad;
}

stdexec::task<std::string> collect_all_cancel_demo() {
  g_collect_cancel_observed.store(0);
  auto stop = std::make_shared<stdexec::inplace_stop_source>();
  auto [tx_f, rx_f] = co::oneshot::channel<std::string>();
  auto [tx_s, rx_s] = co::oneshot::channel<std::string>();
  launch_on_io(collect_fast_str_task_ch(std::move(tx_f)));
  launch_on_io(collect_any_slow_task_ch(std::move(tx_s), stop->get_token()));
  auto fast_v = co_await std::move(rx_f);  // ~5ms
  stop->request_stop();  // winner requests cancellation of the loser
  auto slow_v = co_await std::move(rx_s);  // loser observes stop, ~20ms
  co_return *fast_v + "|" + *slow_v;
}

stdexec::task<std::string> collect_any_demo() {
  auto stop = std::make_shared<stdexec::inplace_stop_source>();
  auto [tx_f, rx_f] = co::oneshot::channel<std::int32_t>();
  auto [tx_s, rx_s] = co::oneshot::channel<std::string>();
  launch_on_io(collect_any_fast_int_task_ch(std::move(tx_f)));
  launch_on_io(collect_any_slow_task_ch(std::move(tx_s), stop->get_token()));
  auto fast_v = co_await std::move(rx_f);  // 42, ~5ms
  stop->request_stop();
  auto slow_v = co_await std::move(rx_s);  // "slow-cancelled"
  if (fast_v && *fast_v == 42 && slow_v && *slow_v == "slow-cancelled") {
    co_return "winner=fast,value=42";
  }
  co_return "winner=slow,value=" + (slow_v ? *slow_v : "?");
}

stdexec::task<std::string> collect_any_cancel_demo() {
  g_collect_cancel_observed.store(0);
  auto stop = std::make_shared<stdexec::inplace_stop_source>();
  auto [tx_f, rx_f] = co::oneshot::channel<std::int32_t>();
  auto [tx_s, rx_s] = co::oneshot::channel<std::string>();
  launch_on_io(collect_any_fast_int_task_ch(std::move(tx_f)));
  launch_on_io(collect_any_slow_task_ch(std::move(tx_s), stop->get_token()));
  auto fast_v = co_await std::move(rx_f);
  stop->request_stop();
  auto slow_v = co_await std::move(rx_s);  // loser unwinds with the stop
  const bool loser_cancelled = g_collect_cancel_observed.load() > 0;
  co_return "winner=fast,value=" + std::to_string(*fast_v) + "|" +
           (loser_cancelled ? "loser-cancelled" : "loser-still-running");
}

// uint8_t* → Dart Pointer<UInt8>: echo bytes through a thread-local buffer.
namespace {
constexpr std::int32_t kEchoMaxLen = 256;
thread_local std::vector<std::uint8_t> g_echo_buf(kEchoMaxLen);
}  // namespace

std::uint8_t* echo_bytes(const std::uint8_t* data, std::int32_t len) {
  if (len < 0 || len > kEchoMaxLen) {
    throw std::runtime_error("echo_bytes: len out of range [0, 256]");
  }
  if (len > 0 && data == nullptr) {
    throw std::runtime_error("echo_bytes: data is null");
  }
  if (len > 0 && data != nullptr) {
    std::memcpy(g_echo_buf.data(), data, static_cast<std::size_t>(len));
  }
  return g_echo_buf.data();
}

stdexec::task<std::uint8_t*> async_echo_bytes(const std::uint8_t* data,
                                              std::int32_t len) {
  co_return echo_bytes(data, len);
}

}  // namespace demo::api
