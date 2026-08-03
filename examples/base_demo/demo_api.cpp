#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/dispatch.hpp"
#include "dart_cpp_bridge/error_config.hpp"
#include "dart_cpp_bridge/object_handle.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <stdexec/execution.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dcb {
namespace demo {

enum class MethodId : std::uint32_t {
  kBridgeVersion = 1,
  kAdd = 2,
  kSleepTest = 3,
  kTicks = 4,
  kEcho = 5,
  kFailAsync = 6,
  kFailStream = 7,
  kCallDartHello = 8,
  kCallDartHelloSync = 9,
  kMaybeDouble = 10,
  kSumVec = 11,
  kReverseBytes = 12,
  kNextStatus = 13,
  kSumFixedFour = 14,
  kGreet = 15,
  kScoreTotal = 16,
  kSetSum = 17,
  kNextI128 = 18,
  kTotalAges = 19,
  kCounterCreate = 20,
  kCounterIncrement = 21,
  kCounterGetValue = 22,
  kCounterDrop = 23,
  kCounterValueSync = 24,
  kCounterStaticSum = 25,
  kCounterCallDartFn = 26,
  kCounterSleepAndGet = 27,
  kCounterIncrementStream = 28,
  kCounterCreateDefault = 29,
  kCounterZero = 30,
  kCounterAddList = 31,
  kCounterSetValue = 32,
  kCounterDuplicate = 33,
  kPairEcho = 34,
  kTupleEcho = 35,
  // Opaque-as-parameter tests
  kCounterAddValues = 36,      // free fn: read two Counters, return sum
  kCounterTransferValue = 37,  // free fn: move value from src to dst
  kCounterSumHandles = 38,     // free fn: sum a list of Counter handles
  kCounterCloneFrom = 39,      // free fn: create new Counter from existing
  kCounterConsumeAndNew = 40,  // free fn: drop original, return new (move semantics)
};

std::int32_t bridge_version() { return 1; }

// ---------------------------------------------------------------------------
// Async business functions — std::exec style: each returns a *sender*.
// Errors (exceptions inside then/functions) are delivered as set_error and
// surface via the dispatch receiver.
// ---------------------------------------------------------------------------

stdexec::sender auto add(std::int32_t a, std::int32_t b) { return stdexec::just(a + b); }

std::string sleep_test() {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  return "Done";
}

using I32Sink = decltype(make_i32_sink(nullptr, 0, 0, 0));

void ticks(I32Sink sink, std::int32_t count, std::int32_t interval_ms) {
  asio::post(Runtime::instance().pool(),
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

stdexec::sender auto echo(std::string s) { return stdexec::just(std::move(s)); }

stdexec::sender auto maybe_double(std::optional<std::int32_t> input) {
  return stdexec::just(input.has_value()
                           ? std::optional<std::int32_t>(input.value() * 2)
                           : std::optional<std::int32_t>());
}

stdexec::sender auto sum_vec(std::vector<std::int32_t> values) {
  return stdexec::just(std::accumulate(values.begin(), values.end(), 0));
}

stdexec::sender auto reverse_bytes(std::vector<std::uint8_t> input) {
  std::reverse(input.begin(), input.end());
  return stdexec::just(std::move(input));
}

enum class StatusCode : std::int32_t { kOk = 0, kNotFound = 1, kServerError = 2 };

stdexec::sender auto next_status(StatusCode current) {
  switch (current) {
    case StatusCode::kOk:
      return stdexec::just(StatusCode::kNotFound);
    case StatusCode::kNotFound:
      return stdexec::just(StatusCode::kServerError);
    default:
      return stdexec::just(StatusCode::kOk);
  }
}

stdexec::sender auto sum_fixed_four(std::array<std::int32_t, 4> values) {
  return stdexec::just(std::accumulate(values.begin(), values.end(), 0));
}

struct Person {
  std::string name;
  std::int32_t age;
};

stdexec::sender auto greet(Person person) {
  return stdexec::just(std::string("Hello, ") + person.name + "! You are " +
                       std::to_string(person.age));
}

stdexec::sender auto score_total(std::unordered_map<std::string, std::int32_t> scores) {
  std::int32_t sum = 0;
  for (const auto& [name, score] : scores) {
    sum += score;
  }
  return stdexec::just(sum);
}

stdexec::sender auto set_sum(std::unordered_set<std::int32_t> values) {
  return stdexec::just(std::accumulate(values.begin(), values.end(), 0));
}

stdexec::sender auto pair_echo(std::pair<std::int32_t, std::string> input) {
  return stdexec::just(std::move(input));
}

stdexec::sender auto tuple_echo(std::tuple<std::int32_t, std::string, bool> input) {
  return stdexec::just(std::move(input));
}

stdexec::sender auto echo_i128(Int128 v) { return stdexec::just(v); }

stdexec::sender auto total_ages(std::vector<Person> people) {
  std::int32_t sum = 0;
  for (const auto& p : people) {
    sum += p.age;
  }
  return stdexec::just(sum);
}

stdexec::sender auto fail_async(std::string message) {
  // throw inside then -> set_error(std::exception_ptr) -> dispatch receiver
  // posts a responseErr frame.
  return stdexec::just(0) | stdexec::then([message = std::move(message)](int) -> int {
           throw std::runtime_error(message.empty() ? "fail_async" : message);
         });
}

void fail_stream(I32Sink sink, std::string message) {
  asio::post(Runtime::instance().pool(),
             [sink = std::move(sink), message = std::move(message)]() mutable {
               sink.add(1);
               sink.error(message.empty() ? "fail_stream" : message);
             });
}

// Counter fixture for hand-written class-method export test.
class Counter {
 public:
  explicit Counter(std::int32_t initial_value) : value_(initial_value) {}

  void increment(std::int32_t delta = 1) { value_ += delta; }
  std::int32_t value() const { return value_; }
  void add_list(const std::vector<std::int32_t>& values) {
    for (const auto v : values) value_ += v;
  }
  void set_value(std::optional<std::int32_t> value) {
    if (value.has_value()) value_ = value.value();
  }

 private:
  std::int32_t value_;
};

std::uint64_t counter_create(std::uint64_t session_id, std::int32_t initial_value) {
  auto obj = std::make_shared<Counter>(initial_value);
  return ObjectHandleRegistry::instance().insert(
      session_id,
      std::static_pointer_cast<void>(obj),
      [](std::shared_ptr<void>&) {
        // shared_ptr destruction handles cleanup.
      });
}

std::uint64_t counter_create_default(std::uint64_t session_id) {
  // Default constructor: Counter() with initial value 0.
  return counter_create(session_id, 0);
}

std::uint64_t counter_zero(std::uint64_t session_id) {
  // Factory constructor as a static method: Counter::zero() -> handle.
  return counter_create(session_id, 0);
}

namespace {

std::shared_ptr<Counter> counter_checked_get(std::uint64_t handle, const char* operation) {
  auto obj = std::static_pointer_cast<Counter>(ObjectHandleRegistry::instance().get(handle));
  if (!obj) {
    throw std::runtime_error(std::string("Counter handle not found or already dropped while ") + operation);
  }
  return obj;
}

std::uint64_t counter_duplicate(std::uint64_t session_id, std::uint64_t handle) {
  auto obj = counter_checked_get(handle, "duplicating");
  return counter_create(session_id, obj->value());
}

// ---------------------------------------------------------------------------
// Opaque-as-parameter free functions.
// These test passing opaque object handles as function parameters (not `this`).
// ---------------------------------------------------------------------------

/// Read two Counter objects and return the sum of their values.
/// Borrow semantics: both handles remain valid after the call.
std::int32_t counter_add_values(std::uint64_t handle_a, std::uint64_t handle_b) {
  auto a = counter_checked_get(handle_a, "addValues(a)");
  auto b = counter_checked_get(handle_b, "addValues(b)");
  return a->value() + b->value();
}

/// Transfer value from src to dst: dst += src.value(). Returns dst's new value.
/// Both handles remain valid (borrow semantics).
std::int32_t counter_transfer_value(std::uint64_t handle_src, std::uint64_t handle_dst) {
  auto src = counter_checked_get(handle_src, "transferValue(src)");
  auto dst = counter_checked_get(handle_dst, "transferValue(dst)");
  dst->increment(src->value());
  return dst->value();
}

/// Sum the values of a list of Counter handles.
/// All handles remain valid after the call (borrow semantics).
std::int32_t counter_sum_handles(const std::vector<std::uint64_t>& handles) {
  std::int32_t sum = 0;
  for (std::size_t i = 0; i < handles.size(); ++i) {
    auto obj = counter_checked_get(handles[i], "sumHandles");
    sum += obj->value();
  }
  return sum;
}

/// Create a new Counter with the same value as the source (clone semantics).
/// Source handle remains valid.
std::uint64_t counter_clone_from(std::uint64_t session_id, std::uint64_t handle) {
  auto obj = counter_checked_get(handle, "cloneFrom");
  return counter_create(session_id, obj->value());
}

/// Consume the original handle (drop it) and create a new Counter with the
/// same value. Simulates FRB move/ownership-transfer semantics:
/// after this call the original handle is invalid.
std::uint64_t counter_consume_and_new(std::uint64_t session_id, std::uint64_t handle) {
  auto obj = counter_checked_get(handle, "consumeAndNew");
  const auto value = obj->value();
  obj.reset();  // release our shared_ptr before dropping
  ObjectHandleRegistry::instance().drop(handle);
  return counter_create(session_id, value);
}

}  // namespace

std::int32_t counter_increment(std::uint64_t handle, std::int32_t delta) {
  auto obj = counter_checked_get(handle, "incrementing");
  obj->increment(delta);
  return obj->value();
}

std::int32_t counter_get_value(std::uint64_t handle) {
  auto obj = counter_checked_get(handle, "reading value");
  return obj->value();
}

std::int32_t counter_value_sync(std::uint64_t handle) {
  // Sync instance method: read the current value directly on the calling thread.
  auto obj = counter_checked_get(handle, "reading value (sync)");
  return obj->value();
}

std::int32_t counter_static_sum(std::int32_t a, std::int32_t b) {
  // Sync static method: no object handle required.
  return a + b;
}

stdexec::sender auto counter_call_dart_fn(std::shared_ptr<Counter> obj,
                                          DartFnStringToString cb) {
  // DartFn callback method: pass the current value as a string to Dart.
  // operator() returns a sender that resolves on the io thread.
  return cb(std::to_string(obj->value()));
}

// ---------------------------------------------------------------------------
// Response plumbing — std::exec receiver style.
// ---------------------------------------------------------------------------

namespace {

void post_ok(const std::shared_ptr<Session>& s, std::uint64_t gen, std::uint64_t req,
             std::uint32_t method, const std::vector<std::uint8_t>& payload) {
  s->try_post(gen, make_frame(MsgType::kResponseOk, req, method, payload));
}

void post_err(const std::shared_ptr<Session>& s, std::uint64_t gen, std::uint64_t req,
              std::uint32_t method, const char* fn, const std::string& msg) {
  ByteWriter w;
  w.i32(1);
  w.str(dcb::error::format(fn, msg));
  s->try_post(gen, make_frame(MsgType::kResponseErr, req, method, w.raw()));
}

// Receiver that turns a sender's completion into a Dart response frame.
// set_value -> responseOk; set_error -> responseErr; set_stopped -> responseErr.
template <typename T>
struct DispatchReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<Session> session;
  std::uint64_t gen{0};
  std::uint64_t req{0};
  std::uint32_t method{0};
  std::string name;
  std::function<void(ByteWriter&, const T&)> encode;

  void set_value(T v) && noexcept {
    try {
      ByteWriter w;
      encode(w, v);
      post_ok(session, gen, req, method, w.raw());
    } catch (const std::exception& e) {
      post_err(session, gen, req, method, name.c_str(), e.what());
    } catch (...) {
      post_err(session, gen, req, method, name.c_str(), "unknown");
    }
  }

  void set_error(std::exception_ptr ep) && noexcept {
    std::string msg = "unknown";
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      msg = e.what();
    } catch (...) {
    }
    post_err(session, gen, req, method, name.c_str(), msg);
  }

  void set_stopped() && noexcept {
    post_err(session, gen, req, method, name.c_str(), "sender stopped");
  }
};

// Run a sender chain on the io scheduler; route its completion into a
// response frame. The business sender's value type must be T. The opstate is
// kept alive until completion (start_detached semantics).
template <typename T, stdexec::sender S, typename Encode>
void run_async(const std::shared_ptr<Session>& session, std::uint64_t gen, std::uint64_t req,
               std::uint32_t method, S&& sndr, Encode&& encode, const char* name) {
  try {
    auto chain = dcb::on_io(std::forward<S>(sndr));
    auto rcvr = DispatchReceiver<T>{
        session, gen, req, method, name,
        std::function<void(ByteWriter&, const T&)>(std::forward<Encode>(encode))};
    dcb::start_detached(std::move(chain), std::move(rcvr));
  } catch (const std::exception& e) {
    post_err(session, gen, req, method, name, e.what());
  } catch (...) {
    post_err(session, gen, req, method, name, "unknown");
  }
}

void run_dart_hello_blocking(const std::shared_ptr<Session>& session, std::uint64_t gen,
                             std::uint64_t req, std::uint32_t method, DartFnStringToString cb) {
  // Offload to pool thread — sync_wait on io would self-deadlock.
  asio::post(Runtime::instance().pool(),
             [session, gen, req, method, cb = std::move(cb)]() mutable {
               try {
                 auto out = dcb::sync_wait(dcb::spawn(cb("Tom")));
                 if (!out) {
                   throw std::runtime_error("DartFn stopped");
                 }
                 ByteWriter w;
                 w.str(std::get<0>(*out));
                 post_ok(session, gen, req, method, w.raw());
               } catch (const std::exception& e) {
                 post_err(session, gen, req, method, "callDartHelloSync", e.what());
               } catch (...) {
                 post_err(session, gen, req, method, "callDartHelloSync", "unknown");
               }
             });
}

void counter_sleep_and_get(std::shared_ptr<Counter> obj, std::int32_t sleep_ms,
                           const std::shared_ptr<Session>& session, std::uint64_t gen,
                           std::uint64_t req, std::uint32_t method) {
  // Normal member method: run blocking work on the thread pool, then post the result back to io.
  auto* io = &Runtime::instance().io();
  asio::post(Runtime::instance().pool(), [obj, sleep_ms, session, gen, req, method, io]() {
    try {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      const auto value = obj->value();
      asio::post(*io, [session, gen, req, method, value]() {
        ByteWriter w;
        w.i32(value);
        post_ok(session, gen, req, method, w.raw());
      });
    } catch (const std::exception& e) {
      asio::post(*io, [session, gen, req, method, msg = std::string(e.what())]() {
        post_err(session, gen, req, method, "Counter::sleepAndGet", msg);
      });
    } catch (...) {
      asio::post(*io, [session, gen, req, method]() {
        post_err(session, gen, req, method, "Counter::sleepAndGet", "unknown");
      });
    }
  });
}

void counter_increment_stream(std::shared_ptr<Counter> obj, std::int32_t count,
                              std::int32_t interval_ms, I32Sink sink) {
  // Stream member method: increment the counter on the thread pool and emit each new value.
  asio::post(Runtime::instance().pool(), [obj, count, interval_ms, sink = std::move(sink)]() mutable {
    try {
      for (std::int32_t i = 0; i < count; ++i) {
        obj->increment(1);
        sink.add(obj->value());
        if (interval_ms > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
      }
      sink.end();
    } catch (const std::exception& e) {
      sink.error(e.what());
    } catch (...) {
      sink.error("unknown");
    }
  });
}

}  // namespace

void dispatch_request(std::shared_ptr<Session> session, std::uint64_t session_id,
                      const std::uint8_t* data, std::size_t len) {
  const auto gen = session->generation();

  FrameHeader frame;
  try {
    frame = parse_frame(data, len);
  } catch (const std::exception& e) {
    post_err(session, gen, 0, 0, "dispatch", std::string("bad frame: ") + e.what());
    return;
  } catch (...) {
    post_err(session, gen, 0, 0, "dispatch", "bad frame");
    return;
  }

  const auto req = frame.request_id;
  const auto method = frame.method_id;

  try {
    switch (static_cast<MethodId>(method)) {
      case MethodId::kBridgeVersion: {
        ByteWriter w;
        w.i32(bridge_version());
        post_ok(session, gen, req, method, w.raw());
        break;
      }
      case MethodId::kAdd: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto a = r.i32();
        const auto b = r.i32();
        run_async<std::int32_t>(session, gen, req, method, add(a, b),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "add");
        break;
      }
      case MethodId::kSleepTest: {
        auto* io = &Runtime::instance().io();
        asio::post(Runtime::instance().pool(), [session, gen, req, method, io]() {
          try {
            auto out = sleep_test();
            asio::post(*io, [session, gen, req, method, out = std::move(out)]() {
              ByteWriter w;
              w.str(out);
              post_ok(session, gen, req, method, w.raw());
            });
          } catch (const std::exception& e) {
            asio::post(*io, [session, gen, req, method, msg = std::string(e.what())]() {
              post_err(session, gen, req, method, "sleepTest", msg);
            });
          } catch (...) {
            asio::post(*io, [session, gen, req, method]() {
              post_err(session, gen, req, method, "sleepTest", "unknown");
            });
          }
        });
        break;
      }
      case MethodId::kTicks: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto count = r.i32();
        const auto interval_ms = r.i32();
        auto sink = make_i32_sink(session, req, gen, method);
        ticks(std::move(sink), count, interval_ms);
        break;
      }
      case MethodId::kEcho: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto s = r.str();
        run_async<std::string>(session, gen, req, method, echo(std::move(s)),
                               [](ByteWriter& w, const std::string& v) { w.str(v); },
                               "echo");
        break;
      }
      case MethodId::kMaybeDouble: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.opt<std::int32_t>([&r]() { return r.i32(); });
        run_async<std::optional<std::int32_t>>(
            session, gen, req, method, maybe_double(input),
            [](ByteWriter& w, const std::optional<std::int32_t>& v) {
              w.opt<std::int32_t>(v, [&w](std::int32_t x) { w.i32(x); });
            },
            "maybeDouble");
        break;
      }
      case MethodId::kSumVec: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto values = r.vec<std::int32_t>([&r]() { return r.i32(); });
        run_async<std::int32_t>(session, gen, req, method, sum_vec(values),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "sumVec");
        break;
      }
      case MethodId::kReverseBytes: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.u8vec();
        run_async<std::vector<std::uint8_t>>(
            session, gen, req, method, reverse_bytes(input),
            [](ByteWriter& w, const std::vector<std::uint8_t>& v) { w.u8vec(v); },
            "reverseBytes");
        break;
      }
      case MethodId::kNextStatus: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto current = r.enume<StatusCode>();
        run_async<StatusCode>(session, gen, req, method, next_status(current),
                              [](ByteWriter& w, const StatusCode& v) { w.enume(v); },
                              "nextStatus");
        break;
      }
      case MethodId::kSumFixedFour: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto values = r.arr<std::int32_t, 4>([&r]() { return r.i32(); });
        run_async<std::int32_t>(session, gen, req, method, sum_fixed_four(values),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "sumFixedFour");
        break;
      }
      case MethodId::kGreet: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        Person person;
        person.name = r.str();
        person.age = r.i32();
        run_async<std::string>(session, gen, req, method, greet(std::move(person)),
                               [](ByteWriter& w, const std::string& v) { w.str(v); },
                               "greet");
        break;
      }
      case MethodId::kScoreTotal: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto scores = r.map<std::string, std::int32_t>(
            [&r]() { return r.str(); }, [&r]() { return r.i32(); });
        run_async<std::int32_t>(session, gen, req, method, score_total(scores),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "scoreTotal");
        break;
      }
      case MethodId::kSetSum: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto values = r.set<std::int32_t>([&r]() { return r.i32(); });
        run_async<std::int32_t>(session, gen, req, method, set_sum(values),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "setSum");
        break;
      }
      case MethodId::kPairEcho: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.pair<std::int32_t, std::string>([&r]() { return r.i32(); }, [&r]() { return r.str(); });
        run_async<std::pair<std::int32_t, std::string>>(
            session, gen, req, method, pair_echo(std::move(input)),
            [](ByteWriter& w, const std::pair<std::int32_t, std::string>& v) {
              w.pair(v, [&w](std::int32_t x) { w.i32(x); },
                     [&w](const std::string& s) { w.str(s); });
            },
            "pairEcho");
        break;
      }
      case MethodId::kTupleEcho: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.tuple<std::int32_t, std::string, bool>(
            [&r]() { return r.i32(); }, [&r]() { return r.str(); }, [&r]() { return r.u8() != 0; });
        run_async<std::tuple<std::int32_t, std::string, bool>>(
            session, gen, req, method, tuple_echo(std::move(input)),
            [](ByteWriter& w, const std::tuple<std::int32_t, std::string, bool>& v) {
              w.tuple(v, [&w](std::int32_t x) { w.i32(x); },
                      [&w](const std::string& s) { w.str(s); },
                      [&w](bool b) { w.u8(b ? 1 : 0); });
            },
            "tupleEcho");
        break;
      }
      case MethodId::kNextI128: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.i128();
        run_async<Int128>(session, gen, req, method, echo_i128(input),
                          [](ByteWriter& w, const Int128& v) { w.i128(v); },
                          "echoI128");
        break;
      }
      case MethodId::kTotalAges: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto people = r.vec<Person>([&r]() {
          Person p;
          p.name = r.str();
          p.age = r.i32();
          return p;
        });
        run_async<std::int32_t>(session, gen, req, method, total_ages(people),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "totalAges");
        break;
      }
      case MethodId::kCounterCreate: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto initial_value = r.i32();
        try {
          auto handle = counter_create(session_id, initial_value);
          ByteWriter w;
          w.u64(handle);
          post_ok(session, gen, req, method, w.raw());
        } catch (const std::exception& e) {
          post_err(session, gen, req, method, "Counter::create", e.what());
        } catch (...) {
          post_err(session, gen, req, method, "Counter::create", "unknown");
        }
        break;
      }
      case MethodId::kCounterIncrement: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto handle = r.u64();
        auto delta = r.i32();
        run_async<std::int32_t>(session, gen, req, method, stdexec::just(counter_increment(handle, delta)),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "Counter::increment");
        break;
      }
      case MethodId::kCounterGetValue: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto handle = r.u64();
        run_async<std::int32_t>(session, gen, req, method, stdexec::just(counter_get_value(handle)),
                                [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
                                "Counter::getValue");
        break;
      }
      case MethodId::kCounterDrop: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto handle = r.u64();
        try {
          ObjectHandleRegistry::instance().drop(handle);
          ByteWriter w;
          post_ok(session, gen, req, method, w.raw());
        } catch (const std::exception& e) {
          post_err(session, gen, req, method, "Counter::drop", e.what());
        } catch (...) {
          post_err(session, gen, req, method, "Counter::drop", "unknown");
        }
        break;
      }
      case MethodId::kCounterCallDartFn: {
        // True async on io: co_await the DartFn sender; io thread free while Dart runs.
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto fn_id = r.u64();
        auto obj = std::static_pointer_cast<Counter>(ObjectHandleRegistry::instance().get(handle));
        if (!obj) {
          post_err(session, gen, req, method, "Counter::callDartFn", "Counter handle not found or already dropped");
          break;
        }
        DartFnStringToString cb(session, gen, fn_id);
        run_async<std::string>(
            session, gen, req, method, counter_call_dart_fn(obj, cb),
            [](ByteWriter& w, const std::string& v) { w.str(v); },
            "Counter::callDartFn");
        break;
      }
      case MethodId::kCounterSleepAndGet: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto sleep_ms = r.i32();
        auto obj = std::static_pointer_cast<Counter>(ObjectHandleRegistry::instance().get(handle));
        if (!obj) {
          post_err(session, gen, req, method, "Counter::sleepAndGet", "Counter handle not found or already dropped");
          break;
        }
        counter_sleep_and_get(obj, sleep_ms, session, gen, req, method);
        break;
      }
      case MethodId::kCounterIncrementStream: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto count = r.i32();
        const auto interval_ms = r.i32();
        auto obj = std::static_pointer_cast<Counter>(ObjectHandleRegistry::instance().get(handle));
        if (!obj) {
          post_err(session, gen, req, method, "Counter::incrementStream", "Counter handle not found or already dropped");
          break;
        }
        auto sink = make_i32_sink(session, req, gen, method);
        counter_increment_stream(obj, count, interval_ms, std::move(sink));
        break;
      }
      case MethodId::kCounterCreateDefault: {
        try {
          auto handle = counter_create_default(session_id);
          ByteWriter w;
          w.u64(handle);
          post_ok(session, gen, req, method, w.raw());
        } catch (const std::exception& e) {
          post_err(session, gen, req, method, "Counter::createDefault", e.what());
        } catch (...) {
          post_err(session, gen, req, method, "Counter::createDefault", "unknown");
        }
        break;
      }
      case MethodId::kCounterZero: {
        try {
          auto handle = counter_zero(session_id);
          ByteWriter w;
          w.u64(handle);
          post_ok(session, gen, req, method, w.raw());
        } catch (const std::exception& e) {
          post_err(session, gen, req, method, "Counter::zero", e.what());
        } catch (...) {
          post_err(session, gen, req, method, "Counter::zero", "unknown");
        }
        break;
      }
      case MethodId::kCounterAddList: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto values = r.vec<std::int32_t>([&r]() { return r.i32(); });
        run_async<std::int32_t>(
            session, gen, req, method,
            stdexec::just(counter_checked_get(handle, "addList")) | stdexec::then(
                [values = std::move(values)](std::shared_ptr<Counter> obj) {
                  obj->add_list(values);
                  return obj->value();
                }),
            [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
            "Counter::addList");
        break;
      }
      case MethodId::kCounterSetValue: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto value = r.opt<std::int32_t>([&r]() { return r.i32(); });
        run_async<std::int32_t>(
            session, gen, req, method,
            stdexec::just(counter_checked_get(handle, "setValue")) | stdexec::then(
                [value](std::shared_ptr<Counter> obj) {
                  obj->set_value(value);
                  return obj->value();
                }),
            [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
            "Counter::setValue");
        break;
      }
      case MethodId::kCounterDuplicate: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        try {
          auto new_handle = counter_duplicate(session_id, handle);
          ByteWriter w;
          w.u64(new_handle);
          post_ok(session, gen, req, method, w.raw());
        } catch (const std::exception& e) {
          post_err(session, gen, req, method, "Counter::duplicate", e.what());
        } catch (...) {
          post_err(session, gen, req, method, "Counter::duplicate", "unknown");
        }
        break;
      }
      case MethodId::kCounterAddValues: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle_a = r.u64();
        const auto handle_b = r.u64();
        run_async<std::int32_t>(
            session, gen, req, method,
            stdexec::just(counter_add_values(handle_a, handle_b)),
            [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
            "counterAddValues");
        break;
      }
      case MethodId::kCounterTransferValue: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle_src = r.u64();
        const auto handle_dst = r.u64();
        run_async<std::int32_t>(
            session, gen, req, method,
            stdexec::just(counter_transfer_value(handle_src, handle_dst)),
            [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
            "counterTransferValue");
        break;
      }
      case MethodId::kCounterSumHandles: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto handles = r.vec<std::uint64_t>([&r]() { return r.u64(); });
        run_async<std::int32_t>(
            session, gen, req, method,
            stdexec::just(counter_sum_handles(handles)),
            [](ByteWriter& w, const std::int32_t& v) { w.i32(v); },
            "counterSumHandles");
        break;
      }
      case MethodId::kCounterCloneFrom: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        try {
          auto new_handle = counter_clone_from(session_id, handle);
          ByteWriter w;
          w.u64(new_handle);
          post_ok(session, gen, req, method, w.raw());
        } catch (const std::exception& e) {
          post_err(session, gen, req, method, "counterCloneFrom", e.what());
        } catch (...) {
          post_err(session, gen, req, method, "counterCloneFrom", "unknown");
        }
        break;
      }
      case MethodId::kCounterConsumeAndNew: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        try {
          auto new_handle = counter_consume_and_new(session_id, handle);
          ByteWriter w;
          w.u64(new_handle);
          post_ok(session, gen, req, method, w.raw());
        } catch (const std::exception& e) {
          post_err(session, gen, req, method, "counterConsumeAndNew", e.what());
        } catch (...) {
          post_err(session, gen, req, method, "counterConsumeAndNew", "unknown");
        }
        break;
      }
      case MethodId::kFailAsync: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto msg = r.str();
        run_async<std::int32_t>(session, gen, req, method, fail_async(std::move(msg)),
                                [](ByteWriter& w, const std::int32_t&) { w.i32(0); },
                                "failAsync");
        break;
      }
      case MethodId::kFailStream: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto msg = r.str();
        auto sink = make_i32_sink(session, req, gen, method);
        fail_stream(std::move(sink), std::move(msg));
        break;
      }
      case MethodId::kCallDartHello: {
        // True async on io: co_await the DartFn sender; io thread free while Dart runs.
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto fn_id = r.u64();
        DartFnStringToString cb(session, gen, fn_id);
        run_async<std::string>(session, gen, req, method, cb("Tom"),
                               [](ByteWriter& w, const std::string& v) { w.str(v); },
                               "callDartHello");
        break;
      }
      case MethodId::kCallDartHelloSync: {
        // Blocking path: offloaded to pool thread (sync_wait on io = deadlock).
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto fn_id = r.u64();
        DartFnStringToString cb(session, gen, fn_id);
        run_dart_hello_blocking(session, gen, req, method, std::move(cb));
        break;
      }
      default:
        post_err(session, gen, req, method, "dispatch", "unknown method");
        break;
    }
  } catch (const std::exception& e) {
    post_err(session, gen, req, method, "dispatch", e.what());
  } catch (...) {
    post_err(session, gen, req, method, "dispatch", "unknown");
  }
}

std::vector<std::uint8_t> dispatch_sync(std::uint64_t /*session_id*/, const std::uint8_t* data, std::size_t len) {
  auto frame = parse_frame(data, len);
  const auto req = frame.request_id;
  const auto method = frame.method_id;

  try {
    switch (static_cast<MethodId>(method)) {
      case MethodId::kBridgeVersion: {
        ByteWriter w;
        w.i32(bridge_version());
        return make_frame(MsgType::kResponseOk, req, method, w.raw());
      }
      case MethodId::kCounterValueSync: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto value = counter_value_sync(handle);
        ByteWriter w;
        w.i32(value);
        return make_frame(MsgType::kResponseOk, req, method, w.raw());
      }
      case MethodId::kCounterStaticSum: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto a = r.i32();
        const auto b = r.i32();
        ByteWriter w;
        w.i32(counter_static_sum(a, b));
        return make_frame(MsgType::kResponseOk, req, method, w.raw());
      }
      default:
        throw std::runtime_error("sync: method not sync-capable");
    }
  } catch (const std::exception& e) {
    ByteWriter w;
    w.i32(1);
    w.str(dcb::error::format("dispatch_sync", e.what()));
    return make_frame(MsgType::kResponseErr, req, method, w.raw());
  } catch (...) {
    ByteWriter w;
    w.i32(1);
    w.str(dcb::error::format("dispatch_sync", "unknown"));
    return make_frame(MsgType::kResponseErr, req, method, w.raw());
  }
}

}  // namespace demo
}  // namespace dcb

// Auto-register dispatch at DLL load time (only when building the shared lib).
#ifdef DCB_REGISTER_DISPATCH
namespace {
const bool _dcb_registered = [] {
  dcb::set_dispatch(&dcb::demo::dispatch_request, &dcb::demo::dispatch_sync);
  return true;
}();
}  // namespace
#endif
