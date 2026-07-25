#pragma once

// Multi-runtime communication API.
//
// Demonstrates non-blocking message passing between the main bridge runtime
// and independent worker runtimes via co::oneshot / co::mpsc channels.

#include "dart_cpp_bridge/annotate.h"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <string>
#include <utility>

namespace multi_rt::api {

/// Start Worker A ("processor") and Worker B ("responder").
/// Each gets its own asio::io_context + thread.
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> start_workers();

/// Stop both workers and release their threads.
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> stop_workers();

/// Send [message] to Worker A for processing.
/// Worker A transforms it and returns via oneshot channel.
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> process_message(std::string message);

/// Ping Worker B with [payload]. Returns a unique response proving
/// cross-runtime oneshot request/reply works.
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> ping_worker(std::string payload);

/// Send [message] through a pipeline: Worker A → Worker B → result.
/// Demonstrates chaining across two independent runtimes.
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> pipeline(std::string message);

/// Fan-out: send [message] to both workers simultaneously.
/// Returns (replyFromA, replyFromB).
BRIDGE_ASYNC
async_simple::coro::Lazy<std::pair<std::string, std::string>> fan_out(
    std::string message);

/// Worker A emits [count] items with [interval_ms] delay between each,
/// forwarded to Dart via mpsc channel → Main → Stream.
void worker_stream(dcb::StreamSink<std::string> sink, std::int32_t count = 5,
                   std::int32_t interval_ms = 50);

}  // namespace multi_rt::api
