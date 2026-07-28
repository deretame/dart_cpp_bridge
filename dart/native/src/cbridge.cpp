// cbridge.cpp — 纯 C 跨运行时桥接 API 实现。
//
// 提供两类功能：
//   1. dcb_invoke_dart_fn — 从任意 C/C++ 代码调用已注册的 Dart 回调
//   2. dcb_async_*       — 让 C++ 协程非阻塞等待外部 C 异步操作完成

#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/cbridge_wait.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ─── 异步操作注册表 ─────────────────────────────────────────────────────────

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

// ─── DartFn 调用内部协程 ────────────────────────────────────────────────────

// MSVC 19.51 协程 lambda 捕获 bug workaround：
// 使用 static 协程函数，通过参数传递所有变量。
static async_simple::coro::Lazy<> cbridge_invoke_coro(
    std::shared_ptr<dcb::Session> session,
    std::uint64_t generation,
    std::uint64_t fn_id,
    std::vector<std::uint8_t> args,
    dcb_dart_fn_callback callback,
    void* userdata) {
  try {
    auto payload = co_await session->invoke_dart_fn_async(generation, fn_id, std::move(args));
    callback(userdata, 1, payload.data(), static_cast<uint32_t>(payload.size()), nullptr);
  } catch (const std::exception& e) {
    callback(userdata, 0, nullptr, 0, e.what());
  } catch (...) {
    callback(userdata, 0, nullptr, 0, "unknown error");
  }
  co_return;
}

// ─── C API 实现 ─────────────────────────────────────────────────────────────

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
  // Sender 析构 → close → Receiver 收到 nullopt → "operation cancelled"
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

  const auto generation = session->generation();
  std::vector<std::uint8_t> args_copy;
  if (args && args_len > 0) {
    args_copy.assign(args, args + args_len);
  }

  dcb::Runtime::instance().spawn_on_asio(
      [session = std::move(session), generation, fn_id,
       args = std::move(args_copy), callback, userdata]() mutable
      -> async_simple::coro::Lazy<> {
        co_await cbridge_invoke_coro(
            std::move(session), generation, fn_id, std::move(args), callback, userdata);
        co_return;
      });

  return 0;
}

}  // extern "C"
