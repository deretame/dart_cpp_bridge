#pragma once

// cbridge_wait.hpp — C++ 协程侧辅助：等待 C 端完成的异步操作。
//
// 配合 cbridge.h 中的 dcb_async_create / dcb_async_complete / dcb_async_fail 使用。
// 典型场景：C++ 协程调用外部 C 库的异步 API，将 op_id 作为 context 传入，
// 外部库完成时调用 dcb_async_complete，协程通过 async_wait 自动恢复。
//
// 用法：
//   uint64_t op = dcb_async_create();
//   external_c_lib_start_work(op, on_done);  // C 库完成后调 dcb_async_complete(op, ...)
//   auto result = co_await dcb::async_wait(op);
//   // result 为 payload bytes；失败时抛 std::runtime_error

#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/channel.hpp"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace dcb {

/// 异步操作的结果。
struct OpResult {
  bool ok{false};
  std::vector<std::uint8_t> data;
  std::string error;
};

namespace detail {
/// 内部：获取 op 的接收端（由 cbridge.cpp 实现）。
/// 如果 op_id 无效返回空的 Receiver。
co::oneshot::Receiver<OpResult> take_async_receiver(std::uint64_t op_id);
}  // namespace detail

/// 在协程中非阻塞等待一个由 dcb_async_create() 创建的异步操作完成。
/// 成功时返回 payload bytes；失败/取消时抛出 std::runtime_error。
///
/// 必须在绑定了 Executor 的 Lazy 中调用（.via(ex)）。
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
