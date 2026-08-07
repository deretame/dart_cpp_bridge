// foreign_api.cpp — Business implementation: libuv worker communicates with the
// bridge via the UvScheduler (stdexec) — no ForeignExecutor, no C registration
// API. Work runs on the uv loop thread with stdexec::starts_on; timers use
// schedule_after; CPU-bound tasks use uv_work.

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
#include <exec/start_detached.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace foreign_demo::api {

namespace {

std::mutex g_mu;
std::unique_ptr<UvWorker> g_uv_worker;

// Fire-and-forget with an error log (exec::start_detached terminates on
// set_error, so detached chains must swallow errors first).
template <stdexec::sender S>
void start_detached_safe(S&& sndr) {
  exec::start_detached(
      std::forward<S>(sndr)
      | stdexec::upon_error([](std::exception_ptr ep) noexcept {
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            std::fprintf(stderr, "[foreign_demo] detached sender error: %s\n", e.what());
          } catch (...) {
            std::fprintf(stderr, "[foreign_demo] detached sender error: unknown\n");
          }
        }));
}

UvScheduler require_worker() {
  if (!g_uv_worker || !g_uv_worker->running()) {
    throw std::runtime_error("uv worker not running");
  }
  return g_uv_worker->scheduler();
}

}  // namespace

stdexec::task<std::string> start_uv_worker() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker) {
    g_uv_worker = std::make_unique<UvWorker>("libuv-worker");
    g_uv_worker->start();
  }
  co_return std::string("uv worker started");
}

stdexec::task<std::string> stop_uv_worker() {
  std::lock_guard lock(g_mu);
  if (g_uv_worker) {
    g_uv_worker->stop();
    g_uv_worker.reset();
  }
  co_return std::string("uv worker stopped");
}

stdexec::task<std::string> ask_uv(std::string message) {
  // Take the scheduler under the lock, then release it before suspending:
  // holding g_mu across co_await would block the io thread (which must stay
  // free to resume this very coroutine).
  UvScheduler sched;
  {
    std::lock_guard lock(g_mu);
    sched = require_worker();
  }
  // Run the work on the uv loop thread; the stdexec::task reschedules the
  // completion back to the bridge io thread (the caller's home scheduler).
  auto result = co_await stdexec::starts_on(
      std::move(sched),
      stdexec::just(std::move(message))
      | stdexec::then([](std::string msg) { return "[uv:" + msg + "]"; }));
  co_return result;
}

stdexec::task<std::int32_t> uv_compute(std::int32_t n) {
  UvScheduler sched;
  {
    std::lock_guard lock(g_mu);
    sched = require_worker();
  }
  // CPU-bound work via uv_queue_work (libuv thread pool); the result is
  // delivered on the uv loop thread and the stdexec::task reschedules it back
  // to the caller's home scheduler (io).
  auto work = sched.uv_work([n] {
    std::int32_t sum = 0;
    for (std::int32_t i = 1; i <= n; ++i) {
      sum += i;
    }
    return sum;
  });
  auto result = co_await stdexec::starts_on(std::move(sched), std::move(work));
  co_return result;
}

// Runs on the uv loop thread: emits `count` items with an async uv timer
// between them, then ends the stream.
static stdexec::task<void> uv_stream_coro(UvScheduler sched,
                                       dcb::StreamSink<std::string> sink,
                                       std::int32_t count, std::int32_t interval_ms) {
  for (std::int32_t i = 0; i < count; ++i) {
    if (interval_ms > 0) {
      co_await sched.schedule_after(std::chrono::milliseconds(interval_ms));
    }
    sink.add("uv_item_" + std::to_string(i));
  }
  sink.end();
  co_return;
}

void uv_stream(dcb::StreamSink<std::string> sink, std::int32_t count,
               std::int32_t interval_ms) {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker || !g_uv_worker->running()) {
    sink.error("uv worker not running");
    return;
  }
  // Fire-and-forget producer on the uv loop thread; the io thread stays free.
  start_detached_safe(stdexec::starts_on(
      g_uv_worker->scheduler(),
      uv_stream_coro(g_uv_worker->scheduler(), std::move(sink), count, interval_ms)));
}

stdexec::task<std::string> call_dart_from_uv(dcb::DartFn<std::string(std::string)> callback,
                                          std::string input) {
  UvScheduler sched;
  {
    std::lock_guard lock(g_mu);
    sched = require_worker();
  }
  // Start the reverse DartFn call on the uv loop thread; dartfn_sender posts
  // to Dart, waits for the oneshot reply and migrates back to the io thread.
  // The outer stdexec::task then reschedules to its home (io). A Dart-side
  // exception surfaces as a C++ error here; convert it to the demo's
  // "ERROR:..." result contract.
  try {
    auto result = co_await stdexec::starts_on(std::move(sched),
                                              std::move(callback)(std::move(input)));
    co_return result;
  } catch (const std::exception& e) {
    co_return std::string("ERROR:") + e.what();
  }
}

// ─── cbridge pure C API tests ────────────────────────────────────────────────

stdexec::task<std::string> test_cbridge_async() {
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

stdexec::task<std::string> test_cbridge_async_fail() {
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

stdexec::task<std::string> test_cbridge_async_cancel() {
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

stdexec::task<std::string> test_cbridge_invoke(dcb::DartFn<std::string(std::string)> callback,
                                            std::string input) {
  // Extract session_id and fn_id from DartFn
  auto session = callback.session();
  if (!session) {
    throw std::runtime_error("test_cbridge_invoke: empty callback");
  }
  uint64_t session_id = dcb::SessionRegistry::instance().find_id(session);
  uint64_t fn_id = callback.fn_id();

  // Encode arguments (using the pure C codec API)
  dcb_writer cw;
  dcb_writer_init(&cw);
  dcb_write_str(&cw, input.c_str());

  // Invoke the pure C API on an independent thread and wait for the callback.
  // Do not block on the io thread (the callback fires on the io thread and would deadlock).
  auto [promise, future] = []{
    std::promise<std::string> p;
    auto f = p.get_future();
    return std::make_pair(std::move(p), std::move(f));
  }();
  auto promise_ptr = std::make_shared<std::promise<std::string>>(std::move(promise));

  // Copy C writer data into a vector (writer lifetime is not tied to the thread)
  std::vector<uint8_t> args(cw.data, cw.data + cw.len);
  dcb_writer_free(&cw);

  std::thread worker([session_id, fn_id, args = std::move(args), promise_ptr] {
    struct Ctx {
      std::shared_ptr<std::promise<std::string>> p;
    };
    auto* ctx = new Ctx{promise_ptr};

    int rc = dcb_invoke_dart_fn(
        session_id, fn_id,
        args.data(), static_cast<uint32_t>(args.size()),
        [](void* ud, int ok, const uint8_t* data, uint32_t data_len, const char* error) {
          auto* c = static_cast<Ctx*>(ud);
          if (ok) {
            // Decode the return value (using the pure C codec API)
            dcb_reader cr;
            dcb_reader_init(&cr, data, data_len);
            uint32_t slen = 0;
            const char* s = dcb_read_str(&cr, &slen);
            c->p->set_value(s ? std::string(s, slen) : std::string());
          } else {
            c->p->set_value(std::string("ERROR:") + (error ? error : "unknown"));
          }
          delete c;
        },
        ctx);

    if (rc != 0) {
      promise_ptr->set_value("ERROR:invoke_failed");
      delete ctx;
    }
  });

  // Wait for the result non-blockingly on the io thread (via spawn_blocking)
  auto result = co_await dcb::spawn_blocking([&future] {
    return future.get();
  });
  worker.join();
  co_return result;
}

// ─── Channel service mode tests ───────────────────────────────────────────────

// Request type: payload + a one-shot reply channel
struct ServiceRequest {
  std::string payload;
  co::oneshot::Sender<std::string> reply_tx;
};

// Service loop: runs on the uv loop thread; stdexec::task reschedules back to the
// uv loop after every recv() completes (the send may come from the io thread).
static stdexec::task<void> service_loop(co::mpsc::Receiver<ServiceRequest> rx) {
  while (auto req = co_await rx.recv()) {
    // Process the request: add a prefix
    std::string result = "[svc:" + req->payload + "]";
    req->reply_tx.send(std::move(result));
  }
  co_return;  // channel closed, service ends
}

stdexec::task<std::string> test_channel_service() {
  // Create the channel and start the service loop under the lock; release it
  // before waiting for replies (never hold g_mu across co_await).
  auto [tx, rx] = co::mpsc::unbounded<ServiceRequest>();
  UvScheduler sched;
  {
    std::lock_guard lock(g_mu);
    sched = require_worker();
    // Start the service loop on the uv loop thread
    start_detached_safe(stdexec::starts_on(std::move(sched), service_loop(std::move(rx))));
  }

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
stdexec::task<std::string> test_channel_service_concurrent() {
  auto [tx, rx] = co::mpsc::unbounded<ServiceRequest>();
  UvScheduler sched;
  {
    std::lock_guard lock(g_mu);
    sched = require_worker();
    start_detached_safe(stdexec::starts_on(std::move(sched), service_loop(std::move(rx))));
  }

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

}  // namespace foreign_demo::api
