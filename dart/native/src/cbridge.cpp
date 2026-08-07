// cbridge.cpp — Pure C cross-runtime bridge API implementation.
//
// Provides two categories of functionality:
//   1. dcb_invoke_dart_fn — call a registered Dart callback from arbitrary C/C++ code
//   2. dcb_async_*       — let C++ coroutines await external C async operations non-blockingly
//
// Both are built on the stdexec migration (see docs/cpp26_executor_model_usage.md):
// the async ops registry holds co::oneshot channels whose receiver side is a
// stdexec sender; dcb_invoke_dart_fn launches an stdexec::task on the runtime's io
// scheduler (starts_on) and the task reschedules back to the io thread after the
// Dart reply lands, so the C callback fires on the io thread as documented.

#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/cbridge_wait.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ─── Async operation registry ─────────────────────────────────────────────────

namespace dcb {
namespace {

struct PendingOp {
  co::oneshot::Sender<OpResult> tx;
  co::oneshot::Receiver<OpResult> rx;
};

struct PendingOps {
  std::mutex mu;
  std::unordered_map<std::uint64_t, PendingOp> ops;
  std::atomic<std::uint64_t> next_id{1};
};

PendingOps& pending_ops() {
  static PendingOps p;
  return p;
}

}  // namespace

namespace detail {

co::oneshot::Receiver<OpResult> take_async_receiver(std::uint64_t op_id) {
  auto& p = pending_ops();
  std::lock_guard lock(p.mu);
  auto it = p.ops.find(op_id);
  if (it == p.ops.end()) {
    return {};
  }
  return std::move(it->second.rx);
}

}  // namespace detail
}  // namespace dcb

// ─── DartFn call internal coroutine ───────────────────────────────────────────

// Invoke the C callback, never letting a throwing user callback escape the
// coroutine: an escaping exception would complete the stdexec::task with set_error
// and re-enter the error path below, double-firing the callback (and the
// noexcept error lambda would then std::terminate on a second throw).
static void fire_dartfn_callback(dcb_dart_fn_callback callback, void* userdata, int ok,
                                 const std::uint8_t* data, std::uint32_t data_len,
                                 const char* error) {
  try {
    callback(userdata, ok, data, data_len, error);
  } catch (...) {
    std::fprintf(stderr, "[cbridge] dartfn user callback threw\n");
  }
}

// MSVC 19.51 coroutine lambda capture bug workaround:
// Use a static coroutine function and pass all variables as parameters.
// stdexec::task: home scheduler = the io scheduler it is starts_on'd from, so the
// callback below always runs on the io thread (the oneshot reply may fire on
// whichever thread called complete_dart_fn; stdexec::task reschedules back home).
static stdexec::task<void> cbridge_invoke_coro(
    std::shared_ptr<dcb::Session> session,
    std::uint64_t generation,
    std::uint64_t fn_id,
    std::vector<std::uint8_t> args,
    dcb_dart_fn_callback callback,
    void* userdata) {
  try {
    auto rx = session->invoke_dart_fn_async(generation, fn_id, std::move(args));
    auto reply = co_await std::move(rx);
    if (!reply) {
      throw std::runtime_error("DartFn: channel closed");
    }
    if (!reply->ok) {
      throw std::runtime_error(reply->error.empty() ? "DartFn failed" : reply->error);
    }
    fire_dartfn_callback(callback, userdata, 1, reply->payload.data(),
                         static_cast<uint32_t>(reply->payload.size()), nullptr);
  } catch (const std::exception& e) {
    fire_dartfn_callback(callback, userdata, 0, nullptr, 0, e.what());
  } catch (...) {
    fire_dartfn_callback(callback, userdata, 0, nullptr, 0, "unknown error");
  }
  co_return;
}

// ─── C API implementation ─────────────────────────────────────────────────────

extern "C" {

DCB_API uint64_t dcb_async_create(void) {
  auto& p = dcb::pending_ops();
  auto [tx, rx] = co::oneshot::channel<dcb::OpResult>();
  const auto id = p.next_id.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(p.mu);
  p.ops.emplace(id, dcb::PendingOp{std::move(tx), std::move(rx)});
  return id;
}

DCB_API void dcb_async_complete(uint64_t op_id, const uint8_t* data, uint32_t len) {
  auto& p = dcb::pending_ops();
  co::oneshot::Sender<dcb::OpResult> tx;
  {
    std::lock_guard lock(p.mu);
    auto it = p.ops.find(op_id);
    if (it == p.ops.end()) return;
    tx = std::move(it->second.tx);
    p.ops.erase(it);
  }
  dcb::OpResult r;
  r.ok = true;
  if (data && len > 0) {
    r.data.assign(data, data + len);
  }
  tx.send(std::move(r));
}

DCB_API void dcb_async_fail(uint64_t op_id, const char* error) {
  auto& p = dcb::pending_ops();
  co::oneshot::Sender<dcb::OpResult> tx;
  {
    std::lock_guard lock(p.mu);
    auto it = p.ops.find(op_id);
    if (it == p.ops.end()) return;
    tx = std::move(it->second.tx);
    p.ops.erase(it);
  }
  dcb::OpResult r;
  r.ok = false;
  r.error = error ? error : "unknown error";
  tx.send(std::move(r));
}

DCB_API void dcb_async_cancel(uint64_t op_id) {
  auto& p = dcb::pending_ops();
  co::oneshot::Sender<dcb::OpResult> tx;
  {
    std::lock_guard lock(p.mu);
    auto it = p.ops.find(op_id);
    if (it == p.ops.end()) return;
    tx = std::move(it->second.tx);
    p.ops.erase(it);
  }
  // Sender destruction -> close -> Receiver receives nullopt -> "operation cancelled"
}

DCB_API int dcb_invoke_dart_fn(
    uint64_t session_id,
    uint64_t fn_id,
    const uint8_t* args,
    uint32_t args_len,
    dcb_dart_fn_callback callback,
    void* userdata) {
  auto session = dcb::SessionRegistry::instance().get(session_id);
  if (!session) {
    return -1;
  }
  if (!callback) {
    return -1;
  }
  if (!dcb::Runtime::instance().running()) {
    return -1;
  }
  // Note: the running() check above is inherently TOCTOU against a concurrent
  // Runtime::stop() — if the runtime stops right after this check, the io task
  // is dropped and the callback never fires (exactly-once becomes exactly-zero).
  // This matches the pre-migration behaviour; callers that need a hard guarantee
  // must not race stop() with in-flight dcb_invoke_dart_fn calls.

  const auto generation = session->generation();
  std::vector<std::uint8_t> args_copy;
  if (args && args_len > 0) {
    args_copy.assign(args, args + args_len);
  }

  // Launch the reverse-call coroutine on the io thread. The stdexec::task reschedules
  // back to the io scheduler after the Dart reply, so the C callback runs on the io
  // thread (documented contract of dcb_dart_fn_callback). The coroutine body catches
  // every exception, so set_error/set_stopped below are defensive (normally
  // unreachable — the oneshot channel has no set_stopped and start_detached has no
  // stop source); they still fire the callback (ok=0) rather than dropping it, to
  // honour cbridge.h's "callback is guaranteed to be called exactly once". Both
  // lambdas run on the io thread (task completion site).
  exec::start_detached(
      stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                         cbridge_invoke_coro(std::move(session), generation, fn_id,
                                             std::move(args_copy), callback, userdata))
      | stdexec::upon_error([callback, userdata](std::exception_ptr ep) noexcept {
          // Log a static message only: the exception text may embed Dart-supplied
          // error strings, which should not be echoed to stderr. The callback
          // still receives the original error text (that is the C API contract).
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            std::fprintf(stderr, "[cbridge] dartfn coroutine error\n");
            fire_dartfn_callback(callback, userdata, 0, nullptr, 0, e.what());
          } catch (...) {
            std::fprintf(stderr, "[cbridge] dartfn coroutine error: unknown\n");
            fire_dartfn_callback(callback, userdata, 0, nullptr, 0, "unknown error");
          }
        })
      | stdexec::upon_stopped([callback, userdata]() noexcept {
          std::fprintf(stderr, "[cbridge] dartfn coroutine stopped\n");
          fire_dartfn_callback(callback, userdata, 0, nullptr, 0, "DartFn: stopped");
        }));

  return 0;
}

}  // extern "C"
