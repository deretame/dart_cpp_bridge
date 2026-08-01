#include "dart_cpp_bridge/asio_executor.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <async_simple/Try.h>
#include <async_simple/Signal.h>
#include <async_simple/coro/Lazy.h>
#include <async_simple/coro/Sleep.h>
#include <async_simple/coro/SyncAwait.h>

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
std::vector<std::uint8_t> dispatch_sync(std::uint64_t session_id, const std::uint8_t* data, std::size_t len);
void dispatch_request(std::shared_ptr<Session> session, std::uint64_t session_id, const std::uint8_t* data, std::size_t len);
}  // namespace dcb::demo

// Demo method ids (same as demo_api.cpp).
enum class MethodId : std::uint32_t {
  kBridgeVersion = 1,
  kAdd = 2,
};

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
      auto out = co_await cb("Tom");
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

// ---------------------------------------------------------------------------
// spawn / spawn_blocking
// ---------------------------------------------------------------------------

// Free coroutine functions: parameters are copied into the coroutine frame,
// which avoids the dangling-lambda-capture problem that a coroutine lambda
// passed by value would have.
async_simple::coro::Lazy<int> return_value(int v) {
  co_return v;
}

async_simple::coro::Lazy<int> signal_and_return(std::shared_ptr<std::promise<int>> done, int v) {
  done->set_value(v);
  co_return v;
}

void test_spawn_fire_and_forget() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<int>>();
  auto fut = done->get_future();
  // Fire-and-forget: start and ignore the result; the coroutine still runs and
  // signals through the promise it captured.
  dcb::spawn_detached(signal_and_return(done, 99));
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn fire-and-forget timed out");
  }
  if (fut.get() != 99) {
    Runtime::instance().stop();
    fail("spawn fire-and-forget wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn fire-and-forget ok\n");
}

void test_spawn_wait_result() {
  using namespace dcb;
  Runtime::instance().start();
  // Block this (main, non-io) thread until the coroutine on io finishes.
  // syncAwait returns the value or rethrows the coroutine's exception — no
  // std::promise/future plumbing needed on the caller side.
  int v = async_simple::coro::syncAwait(dcb::spawn(return_value(7)));
  if (v != 7) {
    Runtime::instance().stop();
    fail("spawn wait wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn wait ok\n");
}

async_simple::coro::Lazy<int> throw_value() {
  if (true) {
    throw std::runtime_error("sync-boom");
  }
  co_return 0;
}

void test_spawn_syncawait_exception() {
  using namespace dcb;
  Runtime::instance().start();
  std::string what;
  try {
    (void)async_simple::coro::syncAwait(dcb::spawn(throw_value()));
    what = "no-throw";
  } catch (const std::exception& e) {
    what = e.what();
  }
  if (what != "sync-boom") {
    Runtime::instance().stop();
    fail("spawn syncAwait exception not propagated");
  }
  Runtime::instance().stop();
  std::printf("spawn syncAwait exception propagation ok\n");
}

void test_syncawait_rejected_on_io_thread() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<bool>>();
  auto fut = done->get_future();
  // Run a coroutine ON the io thread that tries to syncAwait another io-bound
  // Lazy. The deadlock guard (AsioExecutor::currentThreadInExecutor) must
  // reject it with std::logic_error instead of letting the io thread block on
  // itself (which would deadlock and hang the runtime).
  Runtime::instance().spawn_on_asio([done]() -> async_simple::coro::Lazy<> {
    bool rejected = false;
    try {
      (void)async_simple::coro::syncAwait(dcb::spawn(return_value(1)));
    } catch (const std::logic_error&) {
      rejected = true;
    }
    done->set_value(rejected);
    co_return;
  });
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("syncAwait-on-io timed out (deadlock guard did not fire)");
  }
  if (!fut.get()) {
    Runtime::instance().stop();
    fail("syncAwait on io thread was not rejected");
  }
  Runtime::instance().stop();
  std::printf("syncAwait rejected on io thread ok\n");
}

void test_spawn_blocking_awaited_no_block_io() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<int>>();
  auto fut = done->get_future();
  std::atomic<int> side_work{0};

  // A coroutine on io co_awaits spawn_blocking (150ms sleep on the pool).
  Runtime::instance().spawn_on_asio([done]() -> async_simple::coro::Lazy<> {
    auto v = co_await dcb::spawn_blocking([] {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      return 42;
    });
    done->set_value(v);
    co_return;
  });

  // While the pool thread is sleeping, io must stay responsive.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  for (int i = 0; i < 5; ++i) {
    asio::post(Runtime::instance().io(), [&] { side_work.fetch_add(1); });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  if (side_work.load() != 5) {
    Runtime::instance().stop();
    fail("io blocked while spawn_blocking awaiting");
  }
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking awaited timed out");
  }
  if (fut.get() != 42) {
    Runtime::instance().stop();
    fail("spawn_blocking awaited wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking awaited (io not blocked) ok\n");
}

void test_spawn_blocking_fire_and_forget() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<int>>();
  auto fut = done->get_future();
  dcb::spawn_blocking([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return 123;
  })
      .via(Runtime::instance().executor())
      .start([done](async_simple::Try<int>&& t) {
        try {
          if (t.hasError()) {
            done->set_exception(t.getException());
          } else {
            done->set_value(t.value());
          }
        } catch (...) {
        }
      });
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking fire-and-forget timed out");
  }
  if (fut.get() != 123) {
    Runtime::instance().stop();
    fail("spawn_blocking fire-and-forget wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking fire-and-forget ok\n");
}

void test_spawn_blocking_exception() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<std::string>>();
  auto fut = done->get_future();
  Runtime::instance().spawn_on_asio([done]() -> async_simple::coro::Lazy<> {
    try {
      co_await dcb::spawn_blocking([]() -> int { throw std::runtime_error("boom"); });
      done->set_value("no-throw");
    } catch (const std::exception& e) {
      done->set_value(e.what());
    }
    co_return;
  });
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking exception timed out");
  }
  if (fut.get() != "boom") {
    Runtime::instance().stop();
    fail("spawn_blocking exception not propagated");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking exception propagation ok\n");
}

void test_spawn_blocking_void() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<bool>>();
  auto fut = done->get_future();
  auto flag = std::make_shared<std::atomic<bool>>(false);
  Runtime::instance().spawn_on_asio([done, flag]() -> async_simple::coro::Lazy<> {
    co_await dcb::spawn_blocking([flag] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      flag->store(true);
    });
    done->set_value(flag->load());
    co_return;
  });
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking void timed out");
  }
  if (!fut.get()) {
    Runtime::instance().stop();
    fail("spawn_blocking void did not run");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking void ok\n");
}

// A void callable that throws must still propagate the exception to the
// awaiter. This exercises the Unit bridge in spawn_blocking: awaiting a
// Future<Unit> goes through Future::value(), which rethrows, whereas a bare
// Future<void> co_await would silently drop the exception.
void test_spawn_blocking_void_exception() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<std::string>>();
  auto fut = done->get_future();
  Runtime::instance().spawn_on_asio([done]() -> async_simple::coro::Lazy<> {
    try {
      co_await dcb::spawn_blocking([]() -> void { throw std::runtime_error("void-boom"); });
      done->set_value("no-throw");
    } catch (const std::exception& e) {
      done->set_value(e.what());
    }
    co_return;
  });
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking void exception timed out");
  }
  if (fut.get() != "void-boom") {
    Runtime::instance().stop();
    fail("spawn_blocking void exception not propagated");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking void exception propagation ok\n");
}

// coro::sleep must suspend the coroutine on a real event-loop timer: the io
// thread stays responsive during the sleep, and the coroutine resumes only
// after the requested duration. This exercises the 4-arg
// schedule(Func, Duration, ...) override in AsioExecutor — without it,
// async_simple falls back to spawning a thread that blocks for the whole
// sleep.
void test_coro_sleep_no_block_io() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<long long>>();
  auto fut = done->get_future();
  std::atomic<int> side_work{0};

  auto t0 = std::chrono::steady_clock::now();
  Runtime::instance().spawn_on_asio([done, t0]() -> async_simple::coro::Lazy<> {
    co_await async_simple::coro::sleep(std::chrono::milliseconds(150));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    done->set_value(static_cast<long long>(elapsed));
    co_return;
  });

  // While the coroutine is sleeping on the timer, io must stay responsive.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  for (int i = 0; i < 5; ++i) {
    asio::post(Runtime::instance().io(), [&] { side_work.fetch_add(1); });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  if (side_work.load() != 5) {
    Runtime::instance().stop();
    fail("io blocked while coro::sleep awaiting");
  }
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("coro::sleep timed out");
  }
  auto elapsed = fut.get();
  if (elapsed < 140) {  // small scheduling slack below the requested 150ms
    Runtime::instance().stop();
    fail("coro::sleep resumed too early");
  }
  Runtime::instance().stop();
  std::printf("coro::sleep (io not blocked, ~%lldms) ok\n", elapsed);
}

struct CancellableSleepCtx {
  std::shared_ptr<std::promise<std::string>> done;
  std::shared_ptr<async_simple::Signal> signal;
  std::chrono::steady_clock::time_point t0;
};

// coro::sleep must be interruptible when the coroutine chain is bound to a
// cancellation signal (Lazy::setLazyLocal + SignalType::Terminate). The 4-arg
// schedule(Func, Duration, Slot*) override in AsioExecutor registers a
// timer-cancelling handler on the Slot, so the sleeping coroutine throws
// SignalException promptly instead of waiting out the full 10s.
async_simple::coro::Lazy<> cancellable_sleep_coro(
    std::shared_ptr<CancellableSleepCtx> ctx) {
  try {
    co_await async_simple::coro::sleep(std::chrono::seconds(10))
        .setLazyLocal(ctx->signal.get());
    ctx->done->set_value("not-cancelled");
  } catch (const async_simple::SignalException& e) {
    ctx->done->set_value(e.what());
  }
  co_return;
}

void test_coro_sleep_cancellation() {
  using namespace dcb;
  Runtime::instance().start();
  auto ctx = std::make_shared<CancellableSleepCtx>();
  ctx->done = std::make_shared<std::promise<std::string>>();
  ctx->signal = async_simple::Signal::create();
  ctx->t0 = std::chrono::steady_clock::now();
  auto fut = ctx->done->get_future();

  Runtime::instance().spawn_on_asio(
      [ctx]() { return cancellable_sleep_coro(ctx); });

  // Let the coroutine enter the timer wait, then cancel it.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ctx->signal->emits(async_simple::SignalType::Terminate);

  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("coro::sleep cancellation timed out");
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - ctx->t0)
                     .count();
  const auto msg = fut.get();
  Runtime::instance().stop();
  if (msg.find("timer is canceled") == std::string::npos) {
    fail("coro::sleep did not throw SignalException on cancel");
  }
  if (elapsed >= 2000) {
    fail("coro::sleep cancellation not prompt");
  }
  std::printf("coro::sleep cancellation (cancelled after ~%lldms) ok\n",
              elapsed);
}

struct ForeignSleepCtx {
  std::shared_ptr<std::promise<std::string>> done;
  std::shared_ptr<async_simple::Signal> signal;
  std::chrono::steady_clock::time_point t0;
};

// A ForeignExecutor without native timer callbacks must fall back to the
// waiter-thread implementation, and cancellation must still work there.
async_simple::coro::Lazy<> foreign_sleep_coro(
    std::shared_ptr<ForeignSleepCtx> ctx) {
  try {
    co_await async_simple::coro::sleep(std::chrono::seconds(10))
        .setLazyLocal(ctx->signal.get());
    ctx->done->set_value("not-cancelled");
  } catch (const async_simple::SignalException& e) {
    ctx->done->set_value(e.what());
  }
  co_return;
}

void test_foreign_executor_sleep_fallback_cancel() {
  asio::io_context ioc;
  auto guard = std::make_shared<
      asio::executor_work_guard<asio::io_context::executor_type>>(
      ioc.get_executor());
  std::thread loop([&ioc] { ioc.run(); });

  // Minimal "foreign loop": schedule_fn just posts to an io_context.
  dcb::ForeignExecutor executor(
      "smoke-foreign",
      [](void (*fn)(void*), void* userdata, void* ctx) {
        asio::post(*static_cast<asio::io_context*>(ctx),
                   [fn, userdata] { fn(userdata); });
      },
      &ioc);
  executor.set_loop_thread_id(loop.get_id());

  auto ctx = std::make_shared<ForeignSleepCtx>();
  ctx->done = std::make_shared<std::promise<std::string>>();
  ctx->signal = async_simple::Signal::create();
  ctx->t0 = std::chrono::steady_clock::now();
  auto fut = ctx->done->get_future();

  executor.schedule([&executor, ctx] {
    foreign_sleep_coro(ctx).via(&executor).start([](auto&&) {});
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ctx->signal->emits(async_simple::SignalType::Terminate);

  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    guard->reset();
    loop.join();
    fail("ForeignExecutor fallback sleep cancellation timed out");
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - ctx->t0)
                     .count();
  const auto msg = fut.get();
  guard->reset();
  loop.join();
  if (msg.find("timer is canceled") == std::string::npos) {
    fail("ForeignExecutor fallback sleep did not throw SignalException");
  }
  if (elapsed >= 2000) {
    fail("ForeignExecutor fallback sleep cancellation not prompt");
  }
  std::printf("ForeignExecutor fallback sleep cancellation (~%lldms) ok\n",
              elapsed);
}

struct PendingForeignSleepCtx {
  std::shared_ptr<async_simple::Signal> signal;
  std::atomic<bool> entered{false};
};

// Marks `entered` right before suspending in sleep, so the test can wait
// until the waiter thread exists before destroying the executor.
async_simple::coro::Lazy<> foreign_sleep_pending_coro(
    std::shared_ptr<PendingForeignSleepCtx> ctx) {
  ctx->entered.store(true, std::memory_order_release);
  co_await async_simple::coro::sleep(std::chrono::seconds(10))
      .setLazyLocal(ctx->signal.get());
  co_return;
}

// A ForeignExecutor may be destroyed while a sleep is pending: the waiter
// thread holds shared state (not `this`). When Terminate wakes it afterwards,
// it must see that the executor is gone and exit without posting — no
// use-after-free.
void test_foreign_executor_destroy_with_pending_sleep() {
  asio::io_context ioc;
  auto guard = std::make_shared<
      asio::executor_work_guard<asio::io_context::executor_type>>(
      ioc.get_executor());
  std::thread loop([&ioc] { ioc.run(); });

  auto* executor = new dcb::ForeignExecutor(
      "smoke-foreign-destroy",
      [](void (*fn)(void*), void* userdata, void* ctx) {
        asio::post(*static_cast<asio::io_context*>(ctx),
                   [fn, userdata] { fn(userdata); });
      },
      &ioc);
  executor->set_loop_thread_id(loop.get_id());

  auto ctx = std::make_shared<PendingForeignSleepCtx>();
  ctx->signal = async_simple::Signal::create();
  executor->schedule([executor, ctx] {
    foreign_sleep_pending_coro(ctx).via(executor).start([](auto&&) {});
  });

  // Wait until the coroutine entered the sleep, then give await_suspend time
  // to spawn the waiter thread and register the Terminate handler.
  while (!ctx->entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  delete executor;  // deactivates; the pending waiter must not touch `this`

  // Wake the waiter: it sees the executor is gone and exits without posting.
  ctx->signal->emits(async_simple::SignalType::Terminate);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  guard->reset();
  loop.join();
  std::printf("ForeignExecutor destroy with pending sleep ok\n");
}

}  // namespace

int main() {
  using namespace dcb;
  test_oneshot_cross_thread_wake();
  test_io_not_blocked_while_awaiting();
  test_dartfn_async_e2e_simulated_reply();
  test_spawn_fire_and_forget();
  test_spawn_wait_result();
  test_spawn_syncawait_exception();
  test_syncawait_rejected_on_io_thread();
  test_spawn_blocking_awaited_no_block_io();
  test_spawn_blocking_fire_and_forget();
  test_spawn_blocking_exception();
  test_spawn_blocking_void();
  test_spawn_blocking_void_exception();
  test_coro_sleep_no_block_io();
  test_coro_sleep_cancellation();
  test_foreign_executor_sleep_fallback_cancel();
  test_foreign_executor_destroy_with_pending_sleep();

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
    auto resp = demo::dispatch_sync(0, req.data(), req.size());
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
