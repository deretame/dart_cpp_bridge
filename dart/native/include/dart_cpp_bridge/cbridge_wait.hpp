#pragma once

// cbridge_wait.hpp — C++ coroutine helper: await async operations completed by the C side.
//
// Use together with dcb_async_create / dcb_async_complete / dcb_async_fail from cbridge.h.
// Typical scenario: a C++ coroutine calls an external C library's async API, passes op_id
// as context, and when the external library calls dcb_async_complete, the coroutine resumes
// automatically via async_wait.
//
// Usage:
//   uint64_t op = dcb_async_create();
//   external_c_lib_start_work(op, on_done);  // C lib calls dcb_async_complete(op, ...) when done
//   auto result = co_await dcb::async_wait(op);
//   // result is payload bytes; throws std::runtime_error on failure

#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/channel.hpp"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dcb {

/// Result of an async operation.
struct OpResult {
  bool ok{false};
  std::vector<std::uint8_t> data;
  std::string error;
};

namespace detail {
/// Internal: take the receiver side of an op (implemented by cbridge.cpp).
/// Returns an empty Receiver if op_id is invalid.
co::oneshot::Receiver<OpResult> take_async_receiver(std::uint64_t op_id);
}  // namespace detail

/// Await non-blockingly in a coroutine for an async operation created by dcb_async_create() to complete.
/// Returns payload bytes on success; throws std::runtime_error on failure / cancellation.
///
/// Must be called inside a Lazy bound to an Executor (.via(ex)).
inline async_simple::coro::Lazy<std::vector<std::uint8_t>> async_wait(std::uint64_t op_id) {
  auto rx = detail::take_async_receiver(op_id);
  if (!rx) {
    throw std::runtime_error("async_wait: invalid op_id");
  }
  auto result = co_await rx.recv();
  if (!result) {
    throw std::runtime_error("async_wait: operation cancelled");
  }
  if (!result->ok) {
    throw std::runtime_error(result->error.empty() ? "async operation failed" : result->error);
  }
  co_return std::move(result->data);
}

}  // namespace dcb
