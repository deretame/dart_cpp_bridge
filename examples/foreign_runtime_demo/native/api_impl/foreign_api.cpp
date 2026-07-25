// foreign_api.cpp — 业务实现：libuv worker 通过 ForeignExecutor + channel 与 bridge 通信。

#include "foreign_api.h"

#include "../uv_worker.hpp"

#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace foreign_demo::api {

namespace {

std::mutex g_mu;
std::unique_ptr<UvWorker> g_uv_worker;

}  // namespace

async_simple::coro::Lazy<std::string> start_uv_worker() {
  std::lock_guard lock(g_mu);
  if (!g_uv_worker) {
    g_uv_worker = std::make_unique<UvWorker>("libuv-worker");
    g_uv_worker->start();
  }
  co_return std::string("uv worker started");
}

async_simple::coro::Lazy<std::string> stop_uv_worker() {
  std::lock_guard lock(g_mu);
  if (g_uv_worker) {
    g_uv_worker->stop();
    g_uv_worker.reset();
  }
  co_return std::string("uv worker stopped");
}

async_simple::coro::Lazy<std::string> ask_uv(std::string message) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    // 通过 ForeignExecutor::schedule 投递到 libuv loop 线程。
    // trampoline 在 uv loop 线程上执行 lambda → send 回 bridge。
    // 注意：std::function 要求可拷贝，所以用 shared_ptr 包装 move-only 的 Sender。
    auto* ex = g_uv_worker->executor();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));
    ex->schedule([tx_ptr, msg = std::move(message)]() {
      // 此代码在 libuv loop 线程上执行
      std::string result = "[uv:" + msg + "]";
      tx_ptr->send(std::move(result));  // 非阻塞 send，唤醒 bridge 侧协程
    });
  }

  // 在 bridge 主运行时上等待回复（挂起协程，不阻塞 io 线程）
  auto reply = co_await rx.recv();
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

async_simple::coro::Lazy<std::int32_t> uv_compute(std::int32_t n) {
  auto [tx, rx] = co::oneshot::channel<std::int32_t>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      throw std::runtime_error("uv worker not running");
    }

    auto* ex = g_uv_worker->executor();
    auto tx_ptr = std::make_shared<co::oneshot::Sender<std::int32_t>>(std::move(tx));
    ex->schedule([tx_ptr, n]() {
      // 在 libuv loop 线程上执行计算
      std::int32_t sum = 0;
      for (std::int32_t i = 1; i <= n; ++i) {
        sum += i;
      }
      tx_ptr->send(sum);
    });
  }

  auto reply = co_await rx.recv();
  if (!reply) {
    throw std::runtime_error("uv worker dropped");
  }
  co_return *reply;
}

void uv_stream(dcb::StreamSink<std::string> sink, std::int32_t count,
               std::int32_t interval_ms) {
  auto [tx, rx] = co::mpsc::unbounded<std::string>();

  {
    std::lock_guard lock(g_mu);
    if (!g_uv_worker || !g_uv_worker->running()) {
      sink.error("uv worker not running");
      return;
    }

    // 在 libuv loop 线程上启动发送任务
    // 注意：std::function 要求可拷贝，所以用 shared_ptr 包装 move-only 的 Sender。
    auto* ex = g_uv_worker->executor();
    auto tx_ptr = std::make_shared<co::mpsc::Sender<std::string>>(std::move(tx));
    ex->schedule([tx_ptr, count, interval_ms]() {
      // 在 uv loop 线程上执行（注意：sleep 会阻塞 uv loop，仅用于演示）
      for (std::int32_t i = 0; i < count; ++i) {
        if (interval_ms > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
        if (!tx_ptr->send("uv_item_" + std::to_string(i))) {
          break;
        }
      }
      // tx_ptr 析构 → channel 关闭 → recv 返回 nullopt
    });
  }

  // 在 bridge 主运行时上消费 mpsc 并转发到 StreamSink
  dcb::Runtime::instance().spawn_on_asio(
      [sink = std::move(sink), rx = std::move(rx)]() mutable
      -> async_simple::coro::Lazy<> {
        while (true) {
          auto item = co_await rx.recv();
          if (!item) break;
          sink.add(*item);
        }
        sink.end();
        co_return;
      });
}

}  // namespace foreign_demo::api
