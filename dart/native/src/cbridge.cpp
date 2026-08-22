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
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ─── Async operation registry ─────────────────────────────────────────────────

namespace dcb {
namespace {

struct PendingOp {
  std::shared_ptr<co::oneshot::State<OpResult>> state;
  std::optional<co::oneshot::Receiver<OpResult>> rx;
  bool receiver_taken{false};
  bool completed{false};
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

struct PendingCompletion {
  std::shared_ptr<co::oneshot::State<OpResult>> state;
};

PendingCompletion begin_async_completion(std::uint64_t op_id) {
  auto& p = pending_ops();
  std::lock_guard lock(p.mu);
  auto it = p.ops.find(op_id);
  if (it == p.ops.end()) {
    return {};
  }
  if (it->second.completed) {
    return {};
  }
  it->second.completed = true;
  return {it->second.state};
}

void finish_async_completion(std::uint64_t op_id) {
  auto& p = pending_ops();
  std::lock_guard lock(p.mu);
  auto it = p.ops.find(op_id);
  if (it != p.ops.end() && it->second.receiver_taken) {
    p.ops.erase(it);
  }
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
  if (!it->second.rx) {
    return {};
  }
  auto rx = std::move(*it->second.rx);
  it->second.receiver_taken = true;
  if (it->second.completed) {
    p.ops.erase(it);
  }
  return rx;
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

struct DartFnCallState;

struct PendingDartFnCalls {
  std::mutex mu;
  std::unordered_map<std::uint64_t, std::shared_ptr<DartFnCallState>> calls;
  std::atomic<std::uint64_t> next_id{1};
};

PendingDartFnCalls& pending_dart_fn_calls() {
  // Runtime::~Runtime() invokes the shutdown cancellation hook during static
  // destruction. Keep this registry alive until process exit so that hook
  // does not depend on static-destruction order across translation units.
  static auto* p = new PendingDartFnCalls();
  return *p;
}

struct DartFnCallState : std::enable_shared_from_this<DartFnCallState> {
  DartFnCallState(dcb_dart_fn_callback cb, void* ud) : callback(cb), userdata(ud) {}

  dcb_dart_fn_callback callback;
  void* userdata;
  std::uint64_t pending_id{0};
  std::atomic<bool> completed{false};

  void finish(int ok, const std::uint8_t* data, std::uint32_t data_len,
              const char* error) noexcept {
    if (completed.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    auto& pending = pending_dart_fn_calls();
    {
      std::lock_guard lock(pending.mu);
      pending.calls.erase(pending_id);
    }
    fire_dartfn_callback(callback, userdata, ok, data, data_len, error);
  }
};

std::uint64_t register_pending_dart_fn(const std::shared_ptr<DartFnCallState>& state) {
  auto& pending = pending_dart_fn_calls();
  const auto id = pending.next_id.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(pending.mu);
  state->pending_id = id;
  pending.calls.emplace(id, state);
  return id;
}

namespace dcb {

void cancel_pending_dart_fn_calls() noexcept {
  auto& pending = pending_dart_fn_calls();
  while (true) {
    std::shared_ptr<DartFnCallState> state;
    {
      std::lock_guard lock(pending.mu);
      if (pending.calls.empty()) {
        return;
      }
      auto it = pending.calls.begin();
      state = std::move(it->second);
      pending.calls.erase(it);
    }
    // Shutdown fallback for a task accepted but not yet run by io_. The
    // normal path invokes the same state on the io thread.
    state->finish(0, nullptr, 0, "DartFn: runtime stopped");
  }
}

}  // namespace dcb

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
    std::shared_ptr<DartFnCallState> state) {
  try {
    auto rx = session->invoke_dart_fn_async(generation, fn_id, std::move(args));
    auto reply = co_await std::move(rx);
    if (!reply) {
      throw std::runtime_error("DartFn: channel closed");
    }
    if (!reply->ok) {
      throw std::runtime_error(reply->error.empty() ? "DartFn failed" : reply->error);
    }
    state->finish(1, reply->payload.data(),
                  static_cast<uint32_t>(reply->payload.size()), nullptr);
  } catch (const std::exception& e) {
    state->finish(0, nullptr, 0, e.what());
  } catch (...) {
    state->finish(0, nullptr, 0, "unknown error");
  }
  co_return;
}

// ─── C API implementation ─────────────────────────────────────────────────────

extern "C" {

DCB_API uint64_t dcb_async_create(void) {
  try {
    auto& p = dcb::pending_ops();
    auto state = std::make_shared<co::oneshot::State<dcb::OpResult>>();
    const auto id = p.next_id.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard lock(p.mu);
    p.ops.emplace(id, dcb::PendingOp{
        state, co::oneshot::Receiver<dcb::OpResult>(state), false, false});
    return id;
  } catch (...) {
    return 0;
  }
}

DCB_API void dcb_async_complete(uint64_t op_id, const uint8_t* data, uint32_t len) {
  try {
    auto completion = dcb::begin_async_completion(op_id);
    if (!completion.state) return;
    dcb::OpResult r;
    r.ok = true;
    try {
      if (data && len > 0) {
        r.data.assign(data, data + len);
      }
    } catch (...) {
      r.ok = false;
    }
    try {
      co::oneshot::Sender<dcb::OpResult> tx(completion.state);
      tx.send(std::move(r));
    } catch (...) {
      std::fprintf(stderr, "[cbridge] async completion delivery failed\n");
    }
    dcb::finish_async_completion(op_id);
  } catch (...) {
    std::fprintf(stderr, "[cbridge] dcb_async_complete failed\n");
  }
}

DCB_API void dcb_async_fail(uint64_t op_id, const char* error) {
  try {
    auto completion = dcb::begin_async_completion(op_id);
    if (!completion.state) return;
    dcb::OpResult r;
    r.ok = false;
    try {
      r.error = error ? error : "unknown error";
    } catch (...) {
      // An empty error is valid; async_wait supplies a generic message.
    }
    try {
      co::oneshot::Sender<dcb::OpResult> tx(completion.state);
      tx.send(std::move(r));
    } catch (...) {
      std::fprintf(stderr, "[cbridge] async failure delivery failed\n");
    }
    dcb::finish_async_completion(op_id);
  } catch (...) {
    std::fprintf(stderr, "[cbridge] dcb_async_fail failed\n");
  }
}

DCB_API void dcb_async_cancel(uint64_t op_id) {
  try {
    auto completion = dcb::begin_async_completion(op_id);
    if (!completion.state) return;
    co::oneshot::Sender<dcb::OpResult> tx(completion.state);
    tx.close();
    dcb::finish_async_completion(op_id);
  } catch (...) {
    std::fprintf(stderr, "[cbridge] dcb_async_cancel failed\n");
  }
}

DCB_API int dcb_invoke_dart_fn(
    uint64_t session_id,
    uint64_t fn_id,
    const uint8_t* args,
    uint32_t args_len,
    dcb_dart_fn_callback callback,
    void* userdata) {
  std::shared_ptr<dcb::Session> session;
  try {
    session = dcb::SessionRegistry::instance().get(session_id);
  } catch (...) {
    return -1;
  }
  if (!session) {
    return -1;
  }
  if (!callback) {
    return -1;
  }
  const auto generation = session->generation();
  std::vector<std::uint8_t> args_copy;
  try {
    if (args && args_len > 0) {
      args_copy.assign(args, args + args_len);
    }
  } catch (...) {
    return -1;
  }

  // Launch the reverse-call coroutine on the io thread. The stdexec::task reschedules
  // back to the io scheduler after the Dart reply, so the C callback runs on the io
  // thread (documented contract of dcb_dart_fn_callback). The coroutine body catches
  // every exception, so set_error/set_stopped below are defensive (normally
  // unreachable — the oneshot channel has no set_stopped and start_detached has no
  // stop source); once the request is accepted they still fire the callback
  // (ok=0) rather than dropping it, as documented in cbridge.h. Both
  // lambdas run on the io thread (task completion site).
  std::shared_ptr<DartFnCallState> state;
  bool callback_guaranteed = false;
  try {
    state = std::make_shared<DartFnCallState>(callback, userdata);
    const bool accepted = dcb::Runtime::instance().try_accept([&] {
      register_pending_dart_fn(state);
      callback_guaranteed = true;
      exec::start_detached(
          stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                             cbridge_invoke_coro(std::move(session), generation, fn_id,
                                                 std::move(args_copy), state))
          | stdexec::upon_error([state](std::exception_ptr ep) noexcept {
              // Log a static message only: the exception text may embed
              // Dart-supplied error strings, which should not be echoed to
              // stderr. The callback still receives the original error text.
              try {
                std::rethrow_exception(ep);
              } catch (const std::exception& e) {
                std::fprintf(stderr, "[cbridge] dartfn coroutine error\n");
                state->finish(0, nullptr, 0, e.what());
              } catch (...) {
                std::fprintf(stderr, "[cbridge] dartfn coroutine error: unknown\n");
                state->finish(0, nullptr, 0, "unknown error");
              }
            })
          | stdexec::upon_stopped([state]() noexcept {
              std::fprintf(stderr, "[cbridge] dartfn coroutine stopped\n");
              state->finish(0, nullptr, 0, "DartFn: stopped");
            }));
    });
    if (!accepted) {
      return -1;
    }
  } catch (const std::exception& e) {
    // If registration succeeded but starting the sender failed, finish the
    // same state so shutdown cannot later fire a second callback. A return of
    // zero means the callback contract is active even if startup failed after
    // registration; -1 means the callback was never invoked.
    if (callback_guaranteed) {
      state->finish(0, nullptr, 0, e.what());
      return 0;
    }
    return -1;
  } catch (...) {
    if (callback_guaranteed) {
      state->finish(0, nullptr, 0, "unknown error");
      return 0;
    }
    return -1;
  }

  return 0;
}

}  // extern "C"
