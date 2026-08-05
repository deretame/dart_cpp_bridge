#pragma once

// Foreign runtime demo API — Demonstrates libuv runtime integrating with the
// bridge via a stdexec scheduler (UvScheduler, see uv_scheduler.hpp).
// All async functions return exec::task (or plain senders) and are composed
// with stdexec::starts_on(worker.scheduler(), ...) to run on the uv loop.

#include "dart_cpp_bridge/annotate.h"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <exec/task.hpp>

#include <cstdint>
#include <string>

namespace foreign_demo::api {

/// Start libuv worker (standalone uv_loop_t + thread).
BRIDGE_ASYNC
exec::task<std::string> start_uv_worker();

/// Stop libuv worker.
BRIDGE_ASYNC
exec::task<std::string> stop_uv_worker();

/// Send a message to libuv worker for processing (runs on the uv loop thread).
BRIDGE_ASYNC
exec::task<std::string> ask_uv(std::string message);

/// Get a computed result from libuv worker (demonstrates CPU task on uv thread).
BRIDGE_ASYNC
exec::task<std::int32_t> uv_compute(std::int32_t n);

/// libuv worker sends streaming data to Dart via an mpsc channel.
BRIDGE_NORMAL
void uv_stream(dcb::StreamSink<std::string> sink, std::int32_t count = 5,
               std::int32_t interval_ms = 50);

/// Call a Dart callback from the libuv loop thread (coroutine started on the
/// uv scheduler). The DartFn sender suspends while awaiting the Dart reply and
/// resumes back on the bridge io thread.
BRIDGE_ASYNC
exec::task<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input);

// ─── cbridge pure C API tests ────────────────────────────────────────────────

/// Test dcb_async_create + dcb_async_complete + dcb::async_wait.
/// Creates an async op internally, completes it from a thread after 50ms, and lets the coroutine wait non-blockingly.
BRIDGE_ASYNC
exec::task<std::string> test_cbridge_async();

/// Test dcb_async_fail path.
BRIDGE_ASYNC
exec::task<std::string> test_cbridge_async_fail();

/// Test dcb_async_cancel path.
BRIDGE_ASYNC
exec::task<std::string> test_cbridge_async_cancel();

/// Test dcb_invoke_dart_fn (pure C callback-style Dart function invocation).
/// Extracts session_id/fn_id from DartFn, invokes via the pure C API,
/// and waits for the callback result on an independent thread.
BRIDGE_ASYNC
exec::task<std::string> test_cbridge_invoke(
    dcb::DartFn<std::string(std::string)> callback, std::string input);

/// Test cross-runtime channel service mode:
/// The uv worker runs a long-lived mpsc service loop; the bridge side sends multiple requests and waits for replies.
BRIDGE_ASYNC
exec::task<std::string> test_channel_service();

/// Concurrent version: send 5 requests to mpsc in one batch, then collect all replies.
BRIDGE_ASYNC
exec::task<std::string> test_channel_service_concurrent();

}  // namespace foreign_demo::api
