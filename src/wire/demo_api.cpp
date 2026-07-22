#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <async_simple/coro/Lazy.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dcb {
namespace demo {

// ---- business ----

std::int32_t bridge_version() { return 1; }

async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b) {
  // Placeholder for real async IO; returns on io_context thread.
  co_return a + b;
}

std::string sleep_test() {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  return "Done";
}

using I32Sink = decltype(make_i32_sink(nullptr, 0, 0, 0));

void ticks(I32Sink sink, std::int32_t count, std::int32_t interval_ms) {
  Runtime::instance().spawn_on_asio(
      [sink = std::move(sink), count, interval_ms]() mutable -> async_simple::coro::Lazy<> {
        for (std::int32_t i = 0; i < count; ++i) {
          sink.add(i);
          if (interval_ms > 0) {
            co_await Runtime::instance().spawn_blocking([interval_ms] {
              std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
            });
          }
        }
        sink.end();
        co_return;
      });
}

// ---- wire ----

namespace {

void post_ok(Session* s, std::uint64_t gen, std::uint64_t req, std::uint32_t method,
             const std::vector<std::uint8_t>& payload) {
  s->try_post(gen, make_frame(MsgType::kResponseOk, req, method, payload));
}

void post_err(Session* s, std::uint64_t gen, std::uint64_t req, std::uint32_t method,
              const std::string& msg) {
  ByteWriter w;
  w.i32(1);
  w.str(msg);
  s->try_post(gen, make_frame(MsgType::kResponseErr, req, method, w.raw()));
}

}  // namespace

void dispatch_request(const std::uint8_t* data, std::size_t len) {
  auto& session = global_session();
  const auto gen = session.generation();

  FrameHeader frame;
  try {
    frame = parse_frame(data, len);
  } catch (const std::exception& e) {
    post_err(&session, gen, 0, 0, std::string("bad frame: ") + e.what());
    return;
  } catch (...) {
    post_err(&session, gen, 0, 0, "bad frame");
    return;
  }

  const auto req = frame.request_id;
  const auto method = frame.method_id;

  try {
    switch (static_cast<MethodId>(method)) {
      case MethodId::kBridgeVersion: {
        ByteWriter w;
        w.i32(bridge_version());
        post_ok(&session, gen, req, method, w.raw());
        break;
      }
      case MethodId::kAdd: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto a = r.i32();
        const auto b = r.i32();
        Runtime::instance().spawn_on_asio(
            [gen, req, method, a, b]() -> async_simple::coro::Lazy<> {
              auto& session = global_session();
              try {
                const auto sum = co_await add(a, b);
                ByteWriter w;
                w.i32(sum);
                post_ok(&session, gen, req, method, w.raw());
              } catch (const std::exception& e) {
                post_err(&session, gen, req, method, e.what());
              } catch (...) {
                post_err(&session, gen, req, method, "unknown");
              }
              co_return;
            });
        break;
      }
      case MethodId::kSleepTest: {
        // normal path: blocking pool → post result on io_context (no Lazy await on Future).
        auto* io = &Runtime::instance().io();
        asio::post(Runtime::instance().pool(), [gen, req, method, io]() {
          try {
            auto out = sleep_test();
            asio::post(*io, [gen, req, method, out = std::move(out)]() {
              ByteWriter w;
              w.str(out);
              post_ok(&global_session(), gen, req, method, w.raw());
            });
          } catch (const std::exception& e) {
            asio::post(*io, [gen, req, method, msg = std::string(e.what())]() {
              post_err(&global_session(), gen, req, method, msg);
            });
          } catch (...) {
            asio::post(*io, [gen, req, method]() {
              post_err(&global_session(), gen, req, method, "unknown");
            });
          }
        });
        break;
      }
      case MethodId::kTicks: {
        ByteReader r(frame.payload.data(), frame.payload.size());
        const auto count = r.i32();
        const auto interval_ms = r.i32();
        auto sink = make_i32_sink(&session, req, gen, method);
        ticks(std::move(sink), count, interval_ms);
        break;
      }
      default:
        post_err(&session, gen, req, method, "unknown method");
        break;
    }
  } catch (const std::exception& e) {
    post_err(&session, gen, req, method, e.what());
  } catch (...) {
    post_err(&session, gen, req, method, "unknown");
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
