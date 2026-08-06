#include "foreign_api.h"

#include "uv_worker.hpp"

#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/cbridge_wait.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/dcb_codec.h"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <stdexec/execution.hpp>
#include <stdexec/stop_token.hpp>
#include <exec/start_detached.hpp>
#include <exec/task.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace demo::api {

namespace {

std::mutex g_mu;
std::unique_ptr<UvWorker> g_uv_worker;

// Run a callable on the uv loop thread.
template <class F>
void run_on_uv(UvScheduler sched, F f) {
  exec::start_detached(
      stdexec::starts_on(sched, stdexec::just() | stdexec::then(std::move(f))));
}

// Launch a coroutine/sender on the uv loop thread.
template <class S>
void spawn_on_uv(UvScheduler sched, S&& sndr) {
  exec::start_detached(stdexec::starts_on(sched, std::forward<S>(sndr)));
}

// Launch a coroutine/sender on the bridge io thread.
template <class S>
void spawn_on_io(S&& sndr) {
  exec::start_detached(
      stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                         std::forward<S>(sndr)));
}

}  // namespace

exec::task<std::string> start_uv_worker() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker) {
    g_uv_worker = std::make_unique<UvWorker>("libuv-worker");
    g_uv_worker->start();
  }
  co_return std::string("uv worker started");
}

exec::task<std::string> stop_uv_worker() {
  std::lock_guard lock(g_mu);
  if (g_uv_worker) {
    g_uv_worker->stop();
    g_uv_worker.reset();
  }
  co_return std::string("uv worker stopped");
}

exec::task<std::string> ask_uv(std::string message) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    // Schedule onto the libuv loop thread via the UvScheduler.
    // The trampoline executes a lambda on the uv loop thread → sends back to bridge.
    // Note: the Sender is move-only, so wrap it in shared_ptr for the lambda.
    auto sched = g_uv_worker->scheduler();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));
    run_on_uv(sched, [tx_ptr, msg = std::move(message)]() {
      // This code runs on the libuv loop thread
      std::string result = "[uv:" + msg + "]";
      tx_ptr->send(std::move(result));  // non-blocking send, wakes the bridge-side coroutine
    });
  }

  // Wait for the reply on the bridge main runtime (suspends the coroutine, does not block the io thread)
  auto reply = co_await std::move(rx);
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

exec::task<std::int32_t> uv_compute(std::int32_t n) {
  auto [tx, rx] = co::oneshot::channel<std::int32_t>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    auto sched = g_uv_worker->scheduler();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::int32_t>>(std::move(tx));
    run_on_uv(sched, [tx_ptr, n]() {
      // Compute on the libuv loop thread
      std::int32_t sum = 0;
      for (std::int32_t i = 1; i <= n; ++i) {
        sum += i;
      }
      tx_ptr->send(sum);
    });
  }

  auto reply = co_await std::move(rx);
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

// Forward mpsc items to the Dart StreamSink on the bridge io thread.
// Static coroutine function: MSVC 19.51 coroutine-lambda capture bug workaround.
static exec::task<void> uv_stream_forward(dcb::StreamSink<std::string> sink,
                                          co::mpsc::Receiver<std::string> rx) {
  while (true) {
    auto item = co_await rx.recv();
    if (!item) break;
    sink.add(*item);
  }
  sink.end();
  co_return;
}

void uv_stream(dcb::StreamSink<std::string> sink, std::int32_t count,
               std::int32_t interval_ms) {
  auto [tx, rx] = co::mpsc::unbounded<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      sink.error("uv worker not running");
      return;
    }

    // Start the sender task on the libuv loop thread.
    auto sched = g_uv_worker->scheduler();
    auto tx_ptr = std::make_shared<co::mpsc::Sender<std::string>>(std::move(tx));
    run_on_uv(sched, [tx_ptr, count, interval_ms]() {
      // Run on the uv loop thread (note: sleep blocks the uv loop; for demo only)
      for (std::int32_t i = 0; i < count; ++i) {
        if (interval_ms > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        if (!tx_ptr->send("uv_item_" + std::to_string(i))) {
          break;
        }
      }
      // tx_ptr destruction → channel closes → recv returns nullopt
    });
  }

  // Consume the mpsc on the bridge main runtime and forward to StreamSink.
  spawn_on_io(uv_stream_forward(std::move(sink), std::move(rx)));
}

// Workaround for MSVC 19.51 coroutine lambda capture bug:
// Use a separate static coroutine function and pass all variables as parameters.
static exec::task<void> uv_dart_fn_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  try {
    auto result = co_await cb(input);
    tx_ptr->send(std::move(result));
  } catch (const std::exception& e) {
    tx_ptr->send(std::string("ERROR: ") + e.what());
  }
  co_return;
}

exec::task<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    auto sched = g_uv_worker->scheduler();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

    // Start a coroutine on the uv loop thread that co_awaits the DartFn
    // non-blockingly, then let exec::task reschedule back to the uv loop.
    run_on_uv(sched, [tx_ptr, cb = std::move(callback), input = std::move(input), sched]() mutable {
      spawn_on_uv(sched,
                  uv_dart_fn_coro(std::move(tx_ptr), std::move(cb), std::move(input)));
    });
  }

  // Wait for the result on the bridge main runtime
  auto reply = co_await std::move(rx);
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

// --- UvScheduler sleep (cooperative cancellation via stop token) ---
//
// dcb::sleep() accepts an optional stop token through write_env; when the
// token is requested the timer is cancelled. exec::task propagates
// set_stopped up the coroutine chain without unwinding into catch blocks, so
// this demo polls the token in 20ms slices instead (10s = 500 slices) and
// throws a plain exception, which the caller surfaces as an error reply.

static exec::task<void> uv_sleep_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    std::shared_ptr<stdexec::inplace_stop_source> stop_source, bool cancel,
    std::chrono::milliseconds normal_dur) {
  try {
    if (cancel) {
      for (int i = 0; i < 500; ++i) {  // up to 10s in 20ms slices
        if (stop_source->get_token().stop_requested()) {
          throw std::runtime_error("timer is canceled");
        }
        co_await dcb::sleep(std::chrono::milliseconds(20));
      }
      tx_ptr->send("not-cancelled");
    } else {
      co_await dcb::sleep(normal_dur);
      tx_ptr->send("slept");
    }
  } catch (const std::exception& e) {
    tx_ptr->send(std::string("cancelled:") + e.what());
  }
  co_return;
}

exec::task<std::string> test_foreign_sleep() {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }
    auto sched = g_uv_worker->scheduler();
    auto tx_ptr =
        std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));
    run_on_uv(sched, [tx_ptr, sched]() mutable {
      spawn_on_uv(sched, uv_sleep_coro(std::move(tx_ptr), nullptr, false,
                                       std::chrono::milliseconds(50)));
    });
  }
  auto reply = co_await std::move(rx);
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

exec::task<std::string> test_foreign_sleep_long() {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }
    auto sched = g_uv_worker->scheduler();
    auto tx_ptr =
        std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));
    run_on_uv(sched, [tx_ptr, sched]() mutable {
      spawn_on_uv(sched, uv_sleep_coro(std::move(tx_ptr), nullptr, false,
                                       std::chrono::seconds(10)));
    });
  }
  auto reply = co_await std::move(rx);
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

exec::task<std::string> test_foreign_sleep_cancel() {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  auto stop_source = std::make_shared<stdexec::inplace_stop_source>();
  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }
    auto sched = g_uv_worker->scheduler();
    auto tx_ptr =
        std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));
    run_on_uv(sched, [tx_ptr, stop_source, sched]() mutable {
      spawn_on_uv(sched, uv_sleep_coro(std::move(tx_ptr), std::move(stop_source), true,
                                       std::chrono::milliseconds(50)));
    });
  }
  // Cancel the sleep from a separate thread after ~50ms.
  std::thread canceller([stop_source] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop_source->request_stop();
  });
  canceller.detach();

  auto reply = co_await std::move(rx);
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

// ─── cbridge pure C API tests ────────────────────────────────────────────────

exec::task<std::string> test_cbridge_async() {
  // Create async operation
  uint64_t op = dcb_async_create();

  // Launch a thread that completes the op after 50ms from outside (simulates an external C library callback)
  std::thread completer([op] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const char* msg = "cbridge_ok";
    dcb_async_complete(op, reinterpret_cast<const uint8_t*>(msg), 10);
  });
  completer.detach();

  // Coroutine waits non-blockingly (suspends, does not occupy a thread)
  auto data = co_await dcb::async_wait(op);
  co_return std::string(data.begin(), data.end());
}

exec::task<std::string> test_cbridge_async_fail() {
  uint64_t op = dcb_async_create();

  std::thread failer([op] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dcb_async_fail(op, "intentional_error");
  });
  failer.detach();

  try {
    co_await dcb::async_wait(op);
    co_return std::string("UNEXPECTED_SUCCESS");
  } catch (const std::exception& e) {
    co_return std::string("CAUGHT:") + e.what();
  }
}

exec::task<std::string> test_cbridge_async_cancel() {
  uint64_t op = dcb_async_create();

  std::thread canceller([op] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dcb_async_cancel(op);
  });
  canceller.detach();

  try {
    co_await dcb::async_wait(op);
    co_return std::string("UNEXPECTED_SUCCESS");
  } catch (const std::exception& e) {
    co_return std::string("CAUGHT:") + e.what();
  }
}

exec::task<std::string> test_cbridge_invoke(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  // Extract session_id and fn_id from DartFn
  auto session = callback.session();
  if (!session) {
    throw std::runtime_error("test_cbridge_invoke: empty callback");
  }
  uint64_t session_id = dcb::SessionRegistry::instance().find_id(session);
  uint64_t fn_id = callback.fn_id();

  // Create an async op, then invoke via the C API (async, returns immediately)
  uint64_t op = dcb_async_create();

  dcb_writer w;
  dcb_writer_init(&w);
  dcb_write_str(&w, input.c_str());

  struct invoke_ctx {
    uint64_t op;
  };
  auto* ctx = (struct invoke_ctx*)malloc(sizeof(struct invoke_ctx));
  ctx->op = op;

  int rc = dcb_invoke_dart_fn(
      session_id, fn_id, w.data, w.len,
      [](void* userdata, int ok, const uint8_t* data, uint32_t len,
         const char* error) {
        auto* c = (struct invoke_ctx*)userdata;
        if (ok) {
          dcb_async_complete(c->op, data, len);
        } else {
          // Contract (kept from the pre-migration implementation): a Dart
          // callback that throws is surfaced as an "ERROR:..." string, not
          // as a thrown exception.
          std::string msg = std::string("ERROR:") + (error ? error : "unknown");
          dcb_writer ew;
          dcb_writer_init(&ew);
          dcb_write_str(&ew, msg.c_str());
          dcb_async_complete(c->op, ew.data, ew.len);
          dcb_writer_free(&ew);
        }
        free(c);
      },
      ctx);
  dcb_writer_free(&w);
  if (rc != 0) {
    free(ctx);
    throw std::runtime_error("dcb_invoke_dart_fn failed");
  }

  auto payload = co_await dcb::async_wait(op);
  dcb::ByteReader r(payload.data(), payload.size());
  co_return r.str();
}

// ─── cbridge pure C invocation (no C++ callback type) ────────────────────────

struct cbridge_pure_c_ctx {
  uint64_t op_id;
};

static void on_dart_reply_pure_c(void* userdata, int ok,
                                 const uint8_t* data, uint32_t len,
                                 const char* error) {
  auto* ctx = (struct cbridge_pure_c_ctx*)userdata;
  if (ok) {
    // Decode the Dart return value and prepend the "C:" prefix (established
    // test contract: pure-C layer processes the reply before waking the
    // coroutine).
    dcb_reader r;
    dcb_reader_init(&r, data, len);
    uint32_t slen = 0;
    const char* s = dcb_read_str(&r, &slen);

    char result[512];
    int n = snprintf(result, sizeof(result), "C:%.*s", (int)slen, s ? s : "");
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(result)) n = (int)sizeof(result) - 1;

    dcb_writer w;
    dcb_writer_init(&w);
    dcb_write_str(&w, result);
    dcb_async_complete(ctx->op_id, w.data, w.len);
    dcb_writer_free(&w);
  } else {
    dcb_async_fail(ctx->op_id, error ? error : "dart fn failed");
  }
  free(ctx);
}

// Pure C function: encodes arguments and initiates dcb_invoke_dart_fn.
// Can be called from any thread (here called directly on the io thread because dcb_invoke_dart_fn itself is non-blocking).
static void c_invoke_dart(uint64_t session_id, uint64_t fn_id,
                          uint64_t op_id, const char* input) {
  // Encode arguments to pass to the Dart callback
  dcb_writer w;
  dcb_writer_init(&w);
  dcb_write_str(&w, input);

  // Save context (op_id is needed in the callback)
  struct cbridge_pure_c_ctx* ctx =
      (struct cbridge_pure_c_ctx*)malloc(sizeof(struct cbridge_pure_c_ctx));
  ctx->op_id = op_id;

  // Initiate the call (non-blocking, returns immediately)
  int rc = dcb_invoke_dart_fn(session_id, fn_id,
                              w.data, w.len,
                              on_dart_reply_pure_c, ctx);
  dcb_writer_free(&w);

  if (rc != 0) {
    dcb_async_fail(op_id, "invoke failed: invalid session");
    free(ctx);
  }
}

exec::task<std::string> test_cbridge_invoke_pure_c(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  // 1. Extract IDs required by the pure C API
  auto session = callback.session();
  if (!session) {
    throw std::runtime_error("test_cbridge_invoke_pure_c: empty callback");
  }
  uint64_t session_id = dcb::SessionRegistry::instance().find_id(session);
  uint64_t fn_id = callback.fn_id();

  // 2. Create async operation (used by the C layer to wake this coroutine when done)
  uint64_t op_id = dcb_async_create();

  // 3. Call the pure C function; the C layer decodes/encodes/completes in the callback
  c_invoke_dart(session_id, fn_id, op_id, input.c_str());

  // 4. Suspend and wait for the C layer to complete (does not occupy the io thread)
  auto payload = co_await dcb::async_wait(op_id);

  // 5. Decode the final result returned by the C layer
  dcb::ByteReader r(payload.data(), payload.size());
  co_return r.str();
}

// ─── Pure-C-path dcb_async_cancel test ────────────────────────────────
// Pure C function: cancels the op after a delay (simulates external C layer cancelling an async operation).

static void c_schedule_cancel(uint64_t op_id) {
  std::thread([op_id] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dcb_async_cancel(op_id);
  }).detach();
}

exec::task<std::string> test_cbridge_pure_c_cancel() {
  // 1. Create async operation
  uint64_t op_id = dcb_async_create();

  // 2. Pure C function schedules cancellation from another thread
  c_schedule_cancel(op_id);

  // 3. Coroutine suspends waiting; should receive an "operation cancelled" error
  try {
    co_await dcb::async_wait(op_id);
    co_return std::string("UNEXPECTED_SUCCESS");
  } catch (const std::exception& e) {
    co_return std::string("CAUGHT:") + e.what();
  }
}

// ─── Channel service mode tests ───────────────────────────────────────────────

// Request type: payload + a one-shot reply channel
struct ServiceRequest {
  std::string payload;
  co::oneshot::Sender<std::string> reply_tx;
};

// Service loop: runs for a long time on the uv worker's loop thread
static exec::task<void> service_loop(co::mpsc::Receiver<ServiceRequest> rx) {
  while (auto req = co_await rx.recv()) {
    // Process the request: add a prefix
    std::string result = "[svc:" + req->payload + "]";
    req->reply_tx.send(std::move(result));
  }
  co_return;  // channel closed, service ends
}

exec::task<std::string> test_channel_service() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker || !g_uv_worker->running()) {
    throw std::runtime_error("uv worker not running");
  }

  // Create mpsc channel (bridge side sends, uv worker side receives)
  auto [tx, rx] = co::mpsc::unbounded<ServiceRequest>();

  // Start the service loop on the uv worker's loop thread
  auto sched = g_uv_worker->scheduler();
  spawn_on_uv(sched, service_loop(std::move(rx)));

  // Send 3 requests from the bridge side, each with its own reply channel
  std::string results;
  for (int i = 0; i < 3; ++i) {
    auto [reply_tx, reply_rx] = co::oneshot::channel<std::string>();
    tx.send(ServiceRequest{"msg" + std::to_string(i), std::move(reply_tx)});

    // Wait non-blockingly for the reply (suspends current coroutine, does not occupy the io thread)
    auto reply = co_await std::move(reply_rx);
    if (!reply) throw std::runtime_error("service dropped");
    if (!results.empty()) results += ",";
    results += *reply;
  }

  // Close sender → service loop exits
  tx.close();
  co_return results;  // "[svc:msg0],[svc:msg1],[svc:msg2]"
}

// Concurrent version: send all requests in one batch, then collect all replies.
// Tests mpsc queuing + service loop processing one by one.
exec::task<std::string> test_channel_service_concurrent() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker || !g_uv_worker->running()) {
    throw std::runtime_error("uv worker not running");
  }

  auto [tx, rx] = co::mpsc::unbounded<ServiceRequest>();
  auto sched = g_uv_worker->scheduler();
  spawn_on_uv(sched, service_loop(std::move(rx)));

  // Send 5 requests in one batch first (without waiting) to test mpsc queuing
  std::vector<co::oneshot::Receiver<std::string>> receivers;
  for (int i = 0; i < 5; ++i) {
    auto [reply_tx, reply_rx] = co::oneshot::channel<std::string>();
    tx.send(ServiceRequest{"c" + std::to_string(i), std::move(reply_tx)});
    receivers.push_back(std::move(reply_rx));
  }

  // Then collect all replies (service loop processes them one by one on the uv loop thread)
  std::string results;
  for (auto& reply_rx : receivers) {
    auto reply = co_await std::move(reply_rx);
    if (!reply) throw std::runtime_error("service dropped");
    if (!results.empty()) results += ",";
    results += *reply;
  }

  tx.close();
  co_return results;  // "[svc:c0],[svc:c1],[svc:c2],[svc:c3],[svc:c4]"
}

}  // namespace demo::api
