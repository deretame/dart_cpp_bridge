#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/object_handle.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <async_simple/coro/Lazy.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <unordered_set>

namespace dcb {
namespace demo {

std::int32_t bridge_version() { return 1; }

async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b) { co_return a + b; }

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

async_simple::coro::Lazy<std::string> echo(std::string s) { co_return s; }

async_simple::coro::Lazy<std::optional<std::int32_t>> maybe_double(
    std::optional<std::int32_t> input) {
  if (input.has_value()) {
    co_return input.value() * 2;
  }
  co_return std::nullopt;
}

async_simple::coro::Lazy<std::int32_t> sum_vec(std::vector<std::int32_t> values) {
  std::int32_t sum = 0;
  for (const auto v : values) {
    sum += v;
  }
  co_return sum;
}

async_simple::coro::Lazy<std::vector<std::uint8_t>> reverse_bytes(
    std::vector<std::uint8_t> input) {
  std::reverse(input.begin(), input.end());
  co_return input;
}

enum class StatusCode : std::int32_t { kOk = 0, kNotFound = 1, kServerError = 2 };

async_simple::coro::Lazy<StatusCode> next_status(StatusCode current) {
  switch (current) {
    case StatusCode::kOk:
      co_return StatusCode::kNotFound;
    case StatusCode::kNotFound:
      co_return StatusCode::kServerError;
    default:
      co_return StatusCode::kOk;
  }
}

async_simple::coro::Lazy<std::int32_t> sum_fixed_four(
    std::array<std::int32_t, 4> values) {
  std::int32_t sum = 0;
  for (const auto v : values) {
    sum += v;
  }
  co_return sum;
}

struct Person {
  std::string name;
  std::int32_t age;
};

async_simple::coro::Lazy<std::string> greet(Person person) {
  co_return std::string("Hello, ") + person.name + "! You are " +
            std::to_string(person.age);
}

async_simple::coro::Lazy<std::int32_t> score_total(
    std::unordered_map<std::string, std::int32_t> scores) {
  std::int32_t sum = 0;
  for (const auto& [name, score] : scores) {
    sum += score;
  }
  co_return sum;
}

async_simple::coro::Lazy<std::int32_t> set_sum(std::unordered_set<std::int32_t> values) {
  std::int32_t sum = 0;
  for (const auto v : values) {
    sum += v;
  }
  co_return sum;
}

async_simple::coro::Lazy<Int128> echo_i128(Int128 v) {
  co_return v;
}

async_simple::coro::Lazy<std::int32_t> total_ages(std::vector<Person> people) {
  std::int32_t sum = 0;
  for (const auto& p : people) {
    sum += p.age;
  }
  co_return sum;
}

async_simple::coro::Lazy<std::int32_t> fail_async(std::string message) {
  throw std::runtime_error(message.empty() ? "fail_async" : message);
  co_return 0;
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

async_simple::coro::Lazy<std::string> counter_call_dart_fn(
    std::shared_ptr<Counter> obj, DartFnStringToString cb) {
  // DartFn callback method: pass the current value as a string to Dart.
  co_return co_await cb.callAsync(std::to_string(obj->value()));
}

namespace {

void post_ok(const std::shared_ptr<Session>& s, std::uint64_t gen, std::uint64_t req,
             std::uint32_t method, const std::vector<std::uint8_t>& payload) {
  s->try_post(gen, make_frame(MsgType::kResponseOk, req, method, payload));
}

void post_err(const std::shared_ptr<Session>& s, std::uint64_t gen, std::uint64_t req,
              std::uint32_t method, const std::string& msg) {
  ByteWriter w;
  w.i32(1);
  w.str(msg);
  s->try_post(gen, make_frame(MsgType::kResponseErr, req, method, w.raw()));
}

void run_dart_hello_blocking(const std::shared_ptr<Session>& session, std::uint64_t gen,
                             std::uint64_t req, std::uint32_t method, DartFnStringToString cb) {
  try {
    auto out = cb.callSync("Tom");
    ByteWriter w;
    w.str(out);
    post_ok(session, gen, req, method, w.raw());
  } catch (const std::exception& e) {
    post_err(session, gen, req, method, e.what());
  } catch (...) {
    post_err(session, gen, req, method, "unknown");
  }
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
        post_err(session, gen, req, method, msg);
      });
    } catch (...) {
      asio::post(*io, [session, gen, req, method]() {
        post_err(session, gen, req, method, "unknown");
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
    post_err(session, gen, 0, 0, std::string("bad frame: ") + e.what());
    return;
  } catch (...) {
    post_err(session, gen, 0, 0, "bad frame");
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
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, a, b]() -> async_simple::coro::Lazy<> {
              try {
                const auto sum = co_await add(a, b);
                ByteWriter w;
                w.i32(sum);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
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
              post_err(session, gen, req, method, msg);
            });
          } catch (...) {
            asio::post(*io, [session, gen, req, method]() {
              post_err(session, gen, req, method, "unknown");
            });
          }
        });
        break;
      }
      case MethodId::kTicks: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto count = r.i32();
        const auto interval_ms = r.i32();
        auto sink = make_i32_sink(session.get(), req, gen, method);
        ticks(std::move(sink), count, interval_ms);
        break;
      }
      case MethodId::kEcho: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto s = r.str();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, s = std::move(s)]() -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await echo(std::move(s));
                ByteWriter w;
                w.str(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kMaybeDouble: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.opt<std::int32_t>([&r]() { return r.i32(); });
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, input = std::move(input)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await maybe_double(input);
                ByteWriter w;
                w.opt<std::int32_t>(out, [&w](std::int32_t v) { w.i32(v); });
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kSumVec: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto values = r.vec<std::int32_t>([&r]() { return r.i32(); });
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, values = std::move(values)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await sum_vec(values);
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kReverseBytes: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.u8vec();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, input = std::move(input)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await reverse_bytes(input);
                ByteWriter w;
                w.u8vec(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kNextStatus: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto current = r.enume<StatusCode>();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, current]() -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await next_status(current);
                ByteWriter w;
                w.enume(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kSumFixedFour: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto values = r.arr<std::int32_t, 4>([&r]() { return r.i32(); });
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, values = std::move(values)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await sum_fixed_four(values);
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kGreet: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        Person person;
        person.name = r.str();
        person.age = r.i32();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, person = std::move(person)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await greet(person);
                ByteWriter w;
                w.str(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kScoreTotal: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto scores = r.map<std::string, std::int32_t>(
            [&r]() { return r.str(); }, [&r]() { return r.i32(); });
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, scores = std::move(scores)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await score_total(scores);
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kSetSum: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto values = r.set<std::int32_t>([&r]() { return r.i32(); });
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, values = std::move(values)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await set_sum(values);
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kNextI128: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto input = r.i128();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, input]() -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await echo_i128(input);
                ByteWriter w;
                w.i128(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
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
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, people = std::move(people)]()
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await total_ages(people);
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
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
          post_err(session, gen, req, method, e.what());
        } catch (...) {
          post_err(session, gen, req, method, "unknown");
        }
        break;
      }
      case MethodId::kCounterIncrement: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto handle = r.u64();
        auto delta = r.i32();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, handle, delta]() -> async_simple::coro::Lazy<> {
              try {
                auto out = counter_increment(handle, delta);
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kCounterGetValue: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto handle = r.u64();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, handle]() -> async_simple::coro::Lazy<> {
              try {
                auto out = counter_get_value(handle);
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
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
          post_err(session, gen, req, method, e.what());
        } catch (...) {
          post_err(session, gen, req, method, "unknown");
        }
        break;
      }
      case MethodId::kCounterCallDartFn: {
        // True async on io: co_await oneshot; io thread free while Dart runs.
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto fn_id = r.u64();
        auto obj = std::static_pointer_cast<Counter>(ObjectHandleRegistry::instance().get(handle));
        if (!obj) {
          post_err(session, gen, req, method, "Counter handle not found or already dropped");
          break;
        }
        DartFnStringToString cb(session, gen, fn_id);
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, obj = std::move(obj), cb = std::move(cb)]() mutable
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await counter_call_dart_fn(obj, cb);
                ByteWriter w;
                w.str(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kCounterSleepAndGet: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto handle = r.u64();
        const auto sleep_ms = r.i32();
        auto obj = std::static_pointer_cast<Counter>(ObjectHandleRegistry::instance().get(handle));
        if (!obj) {
          post_err(session, gen, req, method, "Counter handle not found or already dropped");
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
          post_err(session, gen, req, method, "Counter handle not found or already dropped");
          break;
        }
        auto sink = make_i32_sink(session.get(), req, gen, method);
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
          post_err(session, gen, req, method, e.what());
        } catch (...) {
          post_err(session, gen, req, method, "unknown");
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
          post_err(session, gen, req, method, e.what());
        } catch (...) {
          post_err(session, gen, req, method, "unknown");
        }
        break;
      }
      case MethodId::kFailAsync: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto msg = r.str();
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, msg = std::move(msg)]() -> async_simple::coro::Lazy<> {
              try {
                co_await fail_async(std::move(msg));
                post_ok(session, gen, req, method, {});
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kFailStream: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        auto msg = r.str();
        auto sink = make_i32_sink(session.get(), req, gen, method);
        fail_stream(std::move(sink), std::move(msg));
        break;
      }
      case MethodId::kCallDartHello: {
        // True async on io: co_await oneshot; io thread free while Dart runs.
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto fn_id = r.u64();
        DartFnStringToString cb(session, gen, fn_id);
        Runtime::instance().spawn_on_asio(
            [session, gen, req, method, cb = std::move(cb)]() mutable
            -> async_simple::coro::Lazy<> {
              try {
                auto out = co_await cb.callAsync("Tom");
                ByteWriter w;
                w.str(out);
                post_ok(session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
              } catch (...) {
                post_err(session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kCallDartHelloSync: {
        // Sync path: block **this** thread (often io when dispatched from invoke_async).
        // Library does not move you off io. Stalling io is caller's problem.
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto fn_id = r.u64();
        DartFnStringToString cb(session, gen, fn_id);
        run_dart_hello_blocking(session, gen, req, method, std::move(cb));
        break;
      }
      default:
        post_err(session, gen, req, method, "unknown method");
        break;
    }
  } catch (const std::exception& e) {
    post_err(session, gen, req, method, e.what());
  } catch (...) {
    post_err(session, gen, req, method, "unknown");
  }
}

std::vector<std::uint8_t> dispatch_sync(const std::uint8_t* data, std::size_t len) {
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
    w.str(e.what());
    return make_frame(MsgType::kResponseErr, req, method, w.raw());
  } catch (...) {
    ByteWriter w;
    w.i32(1);
    w.str("unknown");
    return make_frame(MsgType::kResponseErr, req, method, w.raw());
  }
}

}  // namespace demo
}  // namespace dcb
