#pragma once

// UvWorker — 基于 libuv 的独立运行时，通过 ForeignExecutor 接入 bridge channel 系统。
//
// 演示非 asio 事件循环如何参与 co::oneshot / co::mpsc 通信：
//   bridge schedule → uv_async_send → loop 线程 drain → 执行协程/任务

#include <uv.h>

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/foreign_runtime.h"

namespace foreign_demo {

class UvWorker {
 public:
  explicit UvWorker(std::string name) : name_(std::move(name)) {}

  ~UvWorker() { stop(); }

  UvWorker(const UvWorker&) = delete;
  UvWorker& operator=(const UvWorker&) = delete;

  void start() {
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);

    uv_loop_init(&loop_);

    // async handle 用于跨线程唤醒 loop
    uv_async_init(&loop_, &async_, [](uv_async_t* handle) {
      auto* self = static_cast<UvWorker*>(handle->data);
      self->drain_pending();
    });
    async_.data = this;

    // 注册到 bridge，获取 ForeignExecutor
    foreign_id_ = dcb_foreign_register(name_.c_str(), &schedule_callback, this);

    // 启动 loop 线程
    thread_ = std::thread([this] {
      // 在 loop 线程上标记自己，使 currentThreadInExecutor() 能正确判断
      dcb_foreign_mark_loop_thread(foreign_id_);
      uv_run(&loop_, UV_RUN_DEFAULT);
    });
  }

  void stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);

    // 注销（之后 bridge 不再向我们 schedule）
    if (foreign_id_) {
      dcb_foreign_unregister(foreign_id_);
      foreign_id_ = 0;
    }

    // 停止 loop
    uv_stop(&loop_);
    uv_async_send(&async_);  // 唤醒使其退出 uv_run

    if (thread_.joinable()) thread_.join();
    uv_loop_close(&loop_);
  }

  bool running() const { return running_.load(std::memory_order_acquire); }
  const std::string& name() const { return name_; }
  uint32_t foreign_id() const { return foreign_id_; }

  /// 获取 ForeignExecutor（用于 channel coAwait / Lazy.via()）
  dcb::ForeignExecutor* executor() {
    return static_cast<dcb::ForeignExecutor*>(dcb_foreign_executor(foreign_id_));
  }

  /// 在 libuv loop 线程上执行一个任务（C++ 便利接口）
  void post(void (*fn)(void*), void* userdata) {
    schedule_callback(fn, userdata, this);
  }

 private:
  /// bridge 的 ForeignExecutor 调用此函数投递任务（静态，C linkage 兼容）
  static void schedule_callback(void (*fn)(void*), void* userdata, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    {
      std::lock_guard lock(self->mu_);
      self->pending_.push({fn, userdata});
    }
    uv_async_send(&self->async_);  // 线程安全唤醒 loop
  }

  /// 在 loop 线程上执行所有待处理任务
  void drain_pending() {
    std::queue<std::pair<void (*)(void*), void*>> batch;
    {
      std::lock_guard lock(mu_);
      batch.swap(pending_);
    }
    while (!batch.empty()) {
      auto [fn, ud] = batch.front();
      batch.pop();
      fn(ud);  // 执行（如协程恢复 trampoline）
    }
  }

  std::string name_;
  uv_loop_t loop_{};
  uv_async_t async_{};
  std::mutex mu_;
  std::queue<std::pair<void (*)(void*), void*>> pending_;
  std::thread thread_;
  uint32_t foreign_id_{0};
  std::atomic<bool> running_{false};
};

}  // namespace foreign_demo
