#pragma once

// cbridge.h — Pure C cross-runtime bridge API.
//
// Provides C/C++ code that does not depend on async-simple / asio with the
// ability to call Dart callbacks and await external operations asynchronously.
// These APIs can be used from any event loop and any thread.
//
// Two categories of functionality:
//   1. dcb_invoke_dart_fn — call a registered Dart callback from arbitrary C/C++ code
//   2. dcb_async_*       — let C++ coroutines await external C async operations non-blockingly
//
// No C++ headers are introduced. Pure C99 compatible.

#include "dart_cpp_bridge/ffi.h"  // DCB_API macro

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── DartFn call (C callback style) ───────────────────────────────────────────

/// Notification function called after a Dart callback completes.
/// Invoked on the bridge's io thread (do not block in this callback).
/// If marshaling to another thread is required, use dcb_async_* or post manually.
///
/// @param userdata   user pointer passed to dcb_invoke_dart_fn
/// @param ok         1=success, 0=failure
/// @param data       encoded return value (wire payload) on success, NULL on failure
/// @param data_len   length of data
/// @param error      error message (NUL-terminated) on failure, NULL on success
typedef void (*dcb_dart_fn_callback)(
    void* userdata,
    int ok,
    const uint8_t* data,
    uint32_t data_len,
    const char* error);

/// Invoke a registered Dart callback function from any thread. Non-blocking.
///
/// @param session_id  target session (ID returned by dcb_session_open)
/// @param fn_id       Dart closure ID (allocated by codegen or manual registration)
/// @param args        encoded arguments (wire payload format), may be NULL
/// @param args_len    length of args
/// @param callback    callback invoked after Dart finishes execution
/// @param userdata    user pointer forwarded to callback
///
/// Returns 0 if the call was successfully initiated, -1 if the session is invalid.
/// The callback is guaranteed to be called exactly once (success or failure).
DCB_API int dcb_invoke_dart_fn(
    uint64_t session_id,
    uint64_t fn_id,
    const uint8_t* args,
    uint32_t args_len,
    dcb_dart_fn_callback callback,
    void* userdata);

// ─── Async operation primitives (C side completes, C++ coroutine side awaits) ─

/// Create an async operation and return its operation ID.
/// The C++ side can use dcb::async_wait(id) to await non-blockingly in a coroutine.
/// The C side calls dcb_async_complete / dcb_async_fail when the operation finishes.
///
/// Typical usage: a C++ coroutine calls an external C library's async API, passes
/// op_id as context, and the external library calls dcb_async_complete when done,
/// resuming the coroutine automatically.
DCB_API uint64_t dcb_async_create(void);

/// Complete an async operation successfully. Can be called from any thread.
/// data/len are the result data (will be copied). op_id becomes invalid after the call.
DCB_API void dcb_async_complete(uint64_t op_id, const uint8_t* data, uint32_t len);

/// Complete an async operation with failure. Can be called from any thread.
/// error is a NUL-terminated error description. op_id becomes invalid after the call.
DCB_API void dcb_async_fail(uint64_t op_id, const char* error);

/// Cancel / release an unfinished async operation.
/// If the C++ side is awaiting, the coroutine receives an "operation cancelled" error.
/// If no one is awaiting, only resources are released. Safe to call on invalid IDs (no-op).
DCB_API void dcb_async_cancel(uint64_t op_id);

#ifdef __cplusplus
}
#endif
