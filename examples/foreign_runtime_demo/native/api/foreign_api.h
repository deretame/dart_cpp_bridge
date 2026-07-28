#pragma once

// Foreign runtime demo API — 演示 libuv 运行时通过 ForeignExecutor 接入 bridge。

#include "dart_cpp_bridge/annotate.h"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <string>

namespace foreign_demo::api {

/// 启动 libuv worker（独立 uv_loop_t + 线程）。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> start_uv_worker();

/// 停止 libuv worker。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> stop_uv_worker();

/// 发送消息到 libuv worker 处理（oneshot channel 跨运行时）。
/// Worker 在 uv loop 线程上执行转换后回复。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> ask_uv(std::string message);

/// 从 libuv worker 获取计算结果（演示 CPU 任务在 uv 线程执行）。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> uv_compute(std::int32_t n);

/// libuv worker 通过 mpsc channel 发送流式数据到 Dart。
void uv_stream(dcb::StreamSink<std::string> sink, std::int32_t count = 5,
               std::int32_t interval_ms = 50);

/// 从 libuv loop 线程调用 Dart 回调（通过 ForeignExecutor 上启动协程）。
/// 协程绑定到 ForeignExecutor，co_await DartFn 时挂起，Dart 回复通过
/// uv_async_send 恢复到 uv loop 线程。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input);

// ─── cbridge 纯 C API 测试 ────────────────────────────────────────────────

/// 测试 dcb_async_create + dcb_async_complete + dcb::async_wait。
/// 内部创建异步操作，启动线程 50ms 后完成，协程非阻塞等待结果。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> test_cbridge_async();

/// 测试 dcb_async_fail 路径。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> test_cbridge_async_fail();

/// 测试 dcb_async_cancel 路径。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> test_cbridge_async_cancel();

/// 测试 dcb_invoke_dart_fn（纯 C 回调风格调用 Dart 函数）。
/// 内部从 DartFn 提取 session_id/fn_id，用纯 C API 调用，
/// 在独立线程上等待回调结果。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> test_cbridge_invoke(
    dcb::DartFn<std::string(std::string)> callback, std::string input);

/// 测试跨运行时 channel 服务模式：
/// uv worker 运行长期 mpsc 服务循环，bridge 侧发送多个请求并等待回复。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> test_channel_service();

/// 并发版本：一次性发送 5 个请求到 mpsc，然后收集所有回复。
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> test_channel_service_concurrent();

}  // namespace foreign_demo::api
