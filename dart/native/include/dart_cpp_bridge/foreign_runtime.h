#pragma once

// Foreign Runtime C API — let non-asio event loops plug into the bridge's channel/coroutine system.
//
// External runtimes (libuv, glib, custom loops, etc.) register a schedule callback to receive
// tasks posted by the bridge (such as coroutine resumptions), enabling non-blocking cross-runtime
// communication.
//
// See docs/foreign_runtime_design.md for details.

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef DART_CPP_BRIDGE_BUILD
#    define DCB_API __declspec(dllexport)
#  else
#    define DCB_API __declspec(dllimport)
#  endif
#else
#  define DCB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Schedule callback type.
/// The bridge uses this callback to post a task to the external runtime.
/// Implementations must ensure fn(userdata) is invoked on the target event-loop thread.
///
/// Parameters:
///   fn       - function to execute (usually a coroutine-resumption trampoline)
///   userdata - argument for fn (heap-allocated; freed inside fn)
///   ctx      - context pointer passed during registration
typedef void (*dcb_schedule_fn)(void (*fn)(void*), void* userdata, void* ctx);

/// Optional delayed-schedule callback type (native timer support).
///
/// A foreign runtime that has its own timer facility (libuv uv_timer_t,
/// glib g_timeout_add, ...) can register this pair so that
/// `co_await async_simple::coro::sleep(...)` on the ForeignExecutor uses a
/// real event-loop timer instead of the thread-based fallback.
///
/// schedule_after_fn:
///   Schedule `fn(userdata)` on the loop thread after at least `delay_us`
///   microseconds. Returns an opaque timer handle, or NULL on failure
///   (the bridge then falls back to a waiter thread; in that case `fn` is
///   guaranteed NOT to be called and `userdata` must not be used).
///   Invoked on the loop thread when the awaiting coroutine runs there
///   (which is the case for co_await sleep on a ForeignExecutor).
///
/// cancel_after_fn:
///   Cancel a pending timer. Must be safe to call from ANY thread
///   (implementations should marshal to their loop thread if needed) and
///   must be a safe no-op for handles that already fired or are unknown.
typedef void* (*dcb_schedule_after_fn)(
    void (*fn)(void*), void* userdata, int64_t delay_us, void* ctx);
typedef void (*dcb_cancel_after_fn)(void* timer_handle, void* ctx);

/// Register an external runtime.
///
/// Parameters:
///   name        - runtime name (for debugging, e.g. "libuv-worker")
///   schedule_fn - callback the bridge uses to post tasks to this runtime
///   ctx         - context passed to schedule_fn (e.g. UvWorker* or uv_loop_t*)
///
/// Returns: runtime_id (>0), used later to fetch the executor or unregister.
///          Returns 0 on failure.
DCB_API uint32_t dcb_foreign_register(const char* name, dcb_schedule_fn schedule_fn, void* ctx);

/// Register an external runtime with optional native timer support.
///
/// Same as dcb_foreign_register, plus:
///   schedule_after_fn - delayed-schedule callback (may be NULL)
///   cancel_after_fn   - timer cancel callback (may be NULL)
///
/// The two timer callbacks must be provided together (both NULL means "no
/// native timer"; the bridge uses the thread-based fallback for sleep).
/// Returns runtime_id (>0), or 0 on failure.
DCB_API uint32_t dcb_foreign_register_ex(
    const char* name,
    dcb_schedule_fn schedule_fn,
    dcb_schedule_after_fn schedule_after_fn,
    dcb_cancel_after_fn cancel_after_fn,
    void* ctx);

/// Unregister an external runtime.
/// After unregistering, any schedule call targeting this runtime becomes a no-op (safe degradation).
/// Already suspended coroutines are not resumed (caller should ensure channels are closed or no longer awaited).
DCB_API void dcb_foreign_unregister(uint32_t runtime_id);

/// Post a task from the external runtime to the bridge's main io_context.
/// Thread-safe and non-blocking. The task executes on the bridge's io thread.
///
/// Typical use: the external runtime sends results back to the bridge after processing a request.
DCB_API void dcb_post_to_bridge(void (*fn)(void*), void* userdata);

/// Get the ForeignExecutor pointer for an external runtime (actual type is dcb::ForeignExecutor*).
/// The returned pointer can be used directly for:
///   - channel coAwait(executor) paths
///   - Lazy.via(executor) binding
///   - any place expecting an async_simple::Executor*
///
/// The pointer's lifetime is tied to runtime_id; do not use it after unregistering.
/// Returns NULL if runtime_id is invalid.
DCB_API void* dcb_foreign_executor(uint32_t runtime_id);

/// Call on the external loop thread to register the current thread as the loop thread for this runtime.
/// Enables ForeignExecutor::currentThreadInExecutor() to return the correct value.
/// Should be called once after the loop thread starts and before any coroutines run.
DCB_API void dcb_foreign_mark_loop_thread(uint32_t runtime_id);

#ifdef __cplusplus
}
#endif
