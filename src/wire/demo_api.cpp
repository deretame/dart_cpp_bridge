#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <async_simple/coro/Lazy.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

}  // namespace

void dispatch_request(std::shared_ptr<Session> session, const std::uint8_t* data,
                      std::size_t len) {
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
        // "Async" path: do NOT block io — wait on thread_pool, then post_ok.
        // (Pool occupation during wait is explicit; not a hidden babysit of callSync.)
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto fn_id = r.u64();
        DartFnStringToString cb(session, gen, fn_id);
        asio::post(Runtime::instance().pool(),
                   [session, gen, req, method, cb = std::move(cb)]() mutable {
                     run_dart_hello_blocking(session, gen, req, method, std::move(cb));
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
  if (static_cast<MethodId>(frame.method_id) != MethodId::kBridgeVersion) {
    throw std::runtime_error("sync: method not sync-capable");
  }
  ByteWriter w;
  w.i32(bridge_version());
  return make_frame(MsgType::kResponseOk, frame.request_id, frame.method_id, w.raw());
}

}  // namespace demo
}  // namespace dcb
