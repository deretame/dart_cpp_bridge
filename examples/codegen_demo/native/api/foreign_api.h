#pragma once

// Foreign runtime demo API — libuv runtime integrated via a plain stdexec
// scheduler (UvScheduler, see foreign_runtime_demo/native/uv_scheduler.hpp).

#include "dart_cpp_bridge/annotate.h"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <exec/task.hpp>

#include <cstdint>
#include <string>

namespace demo::api {

/// Start libuv worker (standalone uv_loop_t + thread).
BRIDGE_ASYNC
exec::task<std::string> start_uv_worker();

/// Stop libuv worker.
BRIDGE_ASYNC
exec::task<std::string> stop_uv_worker();

/// Send a message to libuv worker for processing (oneshot channel across runtimes).
/// The worker processes the message on the uv loop thread and replies.
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
/// UvScheduler). The coroutine is bound to the scheduler, suspends while
/// co_awaiting the DartFn, and the Dart reply resumes on the uv loop thread.
BRIDGE_ASYNC
exec::task<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input);

/// Test dcb::sleep() on the UvScheduler (libuv worker):
/// the coroutine runs on the uv loop, sleeps 50ms, and resumes normally.
BRIDGE_ASYNC
exec::task<std::string> test_foreign_sleep();

/// Test stopping the uv worker while a native timer is still pending:
/// sleeps 10s (normally never completes in tests). stop() must clean the
/// timer up on the loop thread and return promptly.
BRIDGE_ASYNC
exec::task<std::string> test_foreign_sleep_long();

/// Test cancellable sleep on the UvScheduler: a 10s sleep cancelled by a
/// stop token after ~50ms, and the coroutine surfaces the stoppage error.
BRIDGE_ASYNC
exec::task<std::string> test_foreign_sleep_cancel();

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

/// Test a fully pure-C-path dcb_invoke_dart_fn:
/// The callback uses dcb_reader to decode + dcb_writer to encode + dcb_async_complete to wake the coroutine,
/// without any C++ types (no std::promise / std::thread / std::string).
/// Verifies the complete flow described in cbridge.md part 3.
BRIDGE_ASYNC
exec::task<std::string> test_cbridge_invoke_pure_c(
    dcb::DartFn<std::string(std::string)> callback, std::string input);

/// Test pure-C-path dcb_async_cancel:
/// Creates an op then cancels it from a pure C function; the coroutine should receive an "operation cancelled" error.
BRIDGE_ASYNC
exec::task<std::string> test_cbridge_pure_c_cancel();

/// Test cross-runtime channel service mode:
/// The uv worker runs a long-lived mpsc service loop; the bridge side sends multiple requests and waits for replies.
BRIDGE_ASYNC
exec::task<std::string> test_channel_service();

/// Concurrent version: send 5 requests to mpsc in one batch, then collect all replies.
BRIDGE_ASYNC
exec::task<std::string> test_channel_service_concurrent();

}  // namespace demo::api
