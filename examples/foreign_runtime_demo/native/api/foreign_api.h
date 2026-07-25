#pragma once

// Foreign runtime demo API — 演示 libuv 运行时通过 ForeignExecutor 接入 bridge。

#include "dart_cpp_bridge/annotate.h"
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

}  // namespace foreign_demo::api
