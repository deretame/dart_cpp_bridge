#include "dart_cpp_bridge/asio_executor.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <async_simple/Try.h>
#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dcb::demo {
std::vector<std::uint8_t> dispatch_sync(const std::uint8_t* data, std::size_t len);
void dispatch_request(std::shared_ptr<Session> session, std::uint64_t session_id, const std::uint8_t* data, std::size_t len);
}  // namespace dcb::demo

namespace {

struct DartFnSimCtx {
  std::uint64_t sid{0};
  std::shared_ptr<std::promise<std::string>> done;
};

// Kept alive for C function-pointer userdata.
std::shared_ptr<DartFnSimCtx> g_dartfn_sim;
std::shared_ptr<std::promise<int>> g_add_done;

void fail(const char* msg) {
  std::fprintf(stderr, "FAIL: %s\n", msg);
  std::exit(1);
}

void test_oneshot_cross_thread_wake() {
  using namespace dcb;
  asio::io_context ioc;
  AsioExecutor ex(ioc);
  auto guard = asio::make_work_guard(ioc);
  std::thread io_thread([&] { ioc.run(); });

  std::promise<int> done;
  auto fut = done.get_future();
  auto [tx, rx] = co::oneshot::channel<int>();

  auto lazy = [](co::oneshot::Receiver<int> rx) -> async_simple::coro::Lazy<int> {
    auto v = co_await rx.recv();
    if (!v) {
      throw std::runtime_error("oneshot closed");
    }
    co_return *v;
  }(std::move(rx));

  std::move(lazy).via(&ex).start([&done](async_simple::Try<int>&& t) {
    try {
      if (t.hasError()) {
        std::rethrow_exception(t.getException());
      }
      done.set_value(t.value());
    } catch (...) {
      try {
        done.set_exception(std::current_exception());
      } catch (...) {
      }
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::thread sender([tx = std::move(tx)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)tx.send(42);
  });
  sender.join();

  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    guard.reset();
    ioc.stop();
    io_thread.join();
    fail("oneshot cross-thread wake timed out");
  }
  if (fut.get() != 42) {
    guard.reset();
    ioc.stop();
    io_thread.join();
    fail("oneshot value mismatch");
  }

  guard.reset();
  ioc.stop();
  io_thread.join();
  std::printf("oneshot cross-thread wake ok\n");
}

void test_io_not_blocked_while_awaiting() {
  using namespace dcb;
  asio::io_context ioc;
  AsioExecutor ex(ioc);
  auto guard = asio::make_work_guard(ioc);
  std::thread io_thread([&] { ioc.run(); });

  auto [tx, rx] = co::oneshot::channel<int>();
  std::atomic<bool> resumed{false};
  std::atomic<int> side_work{0};

  auto lazy = [](co::oneshot::Receiver<int> rx,
                 std::atomic<bool>* resumed) -> async_simple::coro::Lazy<> {
    auto v = co_await rx.recv();
    (void)v;
    resumed->store(true);
    co_return;
  }(std::move(rx), &resumed);

  std::move(lazy).via(&ex).start([](auto&&) {});
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  for (int i = 0; i < 5; ++i) {
    asio::post(ioc, [&] { side_work.fetch_add(1); });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (side_work.load() != 5) {
    guard.reset();
    ioc.stop();
    io_thread.join();
    fail("io blocked while Lazy awaiting oneshot");
  }
  if (resumed.load()) {
    guard.reset();
    ioc.stop();
    io_thread.join();
    fail("Lazy resumed before send");
  }

  if (!tx.send(1)) {
    fail("send failed");
  }
  for (int i = 0; i < 50 && !resumed.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!resumed.load()) {
    guard.reset();
    ioc.stop();
    io_thread.join();
    fail("Lazy did not resume after send");
  }

  guard.reset();
  ioc.stop();
  io_thread.join();
  std::printf("io not blocked while awaiting ok\n");
}

void test_dartfn_async_e2e_simulated_reply() {
  using namespace dcb;
  Runtime::instance().start();

  auto done = std::make_shared<std::promise<std::string>>();
  auto fut = done->get_future();
  const auto sid = SessionRegistry::instance().open(/*reply_port=*/99);
  g_dartfn_sim = std::make_shared<DartFnSimCtx>(DartFnSimCtx{sid, done});

  Runtime::instance().set_dart_post(
      [](std::int64_t, const std::uint8_t* data, std::size_t len, void*) {
        auto* c = g_dartfn_sim.get();
        if (!c) {
          return;
        }
        try {
          auto h = parse_frame(data, len);
          if (h.type == MsgType::kDartFnCall) {
            const auto reply_id = h.request_id;
            ByteReader r(h.payload.data(), h.payload.size());
            (void)r.u64();
            auto arg = r.str();
            std::thread([sid = c->sid, reply_id, arg = std::move(arg)]() {
              std::this_thread::sleep_for(std::chrono::milliseconds(30));
              ByteWriter w;
              w.str(std::string("Hello, ") + arg + "!");
              auto session = SessionRegistry::instance().get(sid);
              if (session) {
                session->complete_dart_fn(reply_id, true, w.raw(), {});
              }
            }).detach();
            return;
          }
          if (h.type == MsgType::kResponseOk) {
            ByteReader r(h.payload.data(), h.payload.size());
            try {
              c->done->set_value(r.str());
            } catch (...) {
            }
          }
        } catch (const std::exception& e) {
          std::printf("dartfn sim post error: %s\n", e.what());
        }
      },
      nullptr);

  auto session = SessionRegistry::instance().get(sid);
  const auto gen = session->generation();
  Runtime::instance().spawn_on_asio([sid, gen]() -> async_simple::coro::Lazy<> {
    try {
      auto session = SessionRegistry::instance().get(sid);
      if (!session) {
        throw std::runtime_error("no session");
      }
      DartFnStringToString cb(session, gen, /*fn_id=*/1);
      auto out = co_await cb.callAsync("Tom");
      ByteWriter w;
      w.str(out);
      session->try_post(gen, make_frame(MsgType::kResponseOk, 1, 0, w.raw()));
    } catch (const std::exception& e) {
      std::printf("dartfn lazy error: %s\n", e.what());
    }
    co_return;
  });

  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    SessionRegistry::instance().close_all();
    Runtime::instance().set_dart_post(nullptr, nullptr);
    Runtime::instance().stop();
    g_dartfn_sim.reset();
    fail("dartfn async e2e timed out");
  }
  const auto got = fut.get();
  if (got != "Hello, Tom!") {
    SessionRegistry::instance().close_all();
    Runtime::instance().set_dart_post(nullptr, nullptr);
    Runtime::instance().stop();
    g_dartfn_sim.reset();
    fail("dartfn async e2e wrong result");
  }
  std::printf("dartfn async e2e ok: %s\n", got.c_str());
  SessionRegistry::instance().close_all();
  Runtime::instance().set_dart_post(nullptr, nullptr);
  Runtime::instance().stop();
  g_dartfn_sim.reset();
}

}  // namespace

int main() {
  using namespace dcb;
  test_oneshot_cross_thread_wake();
  test_io_not_blocked_while_awaiting();
  test_dartfn_async_e2e_simulated_reply();

  Runtime::instance().start();
  g_add_done = std::make_shared<std::promise<int>>();
  auto add_fut = g_add_done->get_future();
  Runtime::instance().set_dart_post(
      [](std::int64_t, const std::uint8_t* data, std::size_t len, void*) {
        try {
          auto h = parse_frame(data, len);
          std::printf("post type=%u req=%llu payload=%zu\n", static_cast<unsigned>(h.type),
                      static_cast<unsigned long long>(h.request_id), h.payload.size());
          if (h.type == MsgType::kResponseOk && h.request_id == 2 && g_add_done) {
            ByteReader r(h.payload.data(), h.payload.size());
            try {
              g_add_done->set_value(r.i32());
            } catch (...) {
            }
          }
        } catch (const std::exception& e) {
          std::printf("post parse error: %s\n", e.what());
        }
      },
      nullptr);

  auto sid = SessionRegistry::instance().open(/*reply_port=*/1);
  auto session = SessionRegistry::instance().get(sid);

  {
    auto req =
        make_frame(MsgType::kRequest, 1, static_cast<std::uint32_t>(MethodId::kBridgeVersion), {});
    auto resp = demo::dispatch_sync(req.data(), req.size());
    auto h = parse_frame(resp.data(), resp.size());
    ByteReader r(h.payload.data(), h.payload.size());
    std::printf("version=%d\n", r.i32());
  }

  {
    ByteWriter payload;
    payload.i32(40);
    payload.i32(2);
    auto req =
        make_frame(MsgType::kRequest, 2, static_cast<std::uint32_t>(MethodId::kAdd), payload.raw());
    demo::dispatch_request(session, sid, req.data(), req.size());
  }

  if (add_fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    SessionRegistry::instance().close_all();
    Runtime::instance().stop();
    fail("runtime add timed out");
  }
  const int sum = add_fut.get();
  if (sum != 42) {
    SessionRegistry::instance().close_all();
    Runtime::instance().stop();
    fail("runtime add wrong sum");
  }
  std::printf("runtime add ok sum=%d\n", sum);

  SessionRegistry::instance().close_all();
  Runtime::instance().stop();
  g_add_done.reset();
  std::printf("smoke ok\n");
  return 0;
}
