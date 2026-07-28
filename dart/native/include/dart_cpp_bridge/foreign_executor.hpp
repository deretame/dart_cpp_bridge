#pragma once

// ForeignExecutor — 通用外部运行时执行器。
//
// 将 async_simple::Executor::schedule() 转发到用户注册的 C 回调。
// 任何事件循环只需实现 “在 loop 线程上执行一个 void(*)(void*)” 即可接入
// bridge 的 channel / coroutine 系统。
//
// 不依赖 asio。仅需要 async_simple/Executor.h。
// 详见 docs/foreign_runtime_design.md

#include <async_simple/Executor.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace dcb {

class ForeignExecutor : public async_simple::Executor {
 public:
  using ScheduleFn = void (*)(void (*fn)(void*), void* userdata, void* ctx);

  ForeignExecutor(std::string name, ScheduleFn schedule_fn, void* ctx)
      : name_(std::move(name)), schedule_fn_(schedule_fn), ctx_(ctx) {}

  ~ForeignExecutor() override {
    alive_.store(false, std::memory_order_release);
  }

  ForeignExecutor(const ForeignExecutor&) = delete;
  ForeignExecutor& operator=(const ForeignExecutor&) = delete;

  // ─── async_simple::Executor 完整接口 ───────────────────────────────────

  /// 将任务投递到外部运行时。
  /// 内部将 std::function 装箱到堆上，通过 C 函数指针传递。
  /// 外部 loop 线程执行 trampoline 时释放并调用原始函数。
  bool schedule(Func func) override {
    if (!alive_.load(std::memory_order_acquire)) return false;
    auto* boxed = new Func(std::move(func));
    schedule_fn_(&trampoline, boxed, ctx_);
    return true;
  }

  bool schedule(Func func, uint64_t /*schedule_info*/) override {
    return schedule(std::move(func));
  }

  /// 判断当前线程是否是外部 loop 线程。
  /// async_simple 内部多处调用此方法（syncAwait 断言、FutureState 调度等），
  /// 基类默认 throw，必须 override。
  bool currentThreadInExecutor() const override {
    auto id = loop_thread_id_.load(std::memory_order_acquire);
    return id != std::thread::id{} && std::this_thread::get_id() == id;
  }

  async_simple::ExecutorStat stat() const override {
    async_simple::ExecutorStat s;
    return s;
  }

  async_simple::IOExecutor* getIOExecutor() override { return nullptr; }

  // ─── ForeignExecutor 特有接口 ─────────────────────────────────────────

  const std::string& foreign_name() const { return name_; }
  bool alive() const { return alive_.load(std::memory_order_acquire); }

  /// 标记为失效（unregister 时调用）。之后 schedule 返回 false。
  void deactivate() { alive_.store(false, std::memory_order_release); }

  /// 设置外部 loop 线程 ID（启动 loop 线程后调用）。
  /// 使 currentThreadInExecutor() 能正确判断。
  void set_loop_thread_id(std::thread::id id) {
    loop_thread_id_.store(id, std::memory_order_release);
  }

 private:
  /// Trampoline：在外部 loop 线程上执行，释放堆上的 Func 并调用。
  static void trampoline(void* p) {
    auto f = std::unique_ptr<Func>(static_cast<Func*>(p));
    (*f)();
  }

  std::string name_;
  ScheduleFn schedule_fn_;
  void* ctx_;
  std::atomic<bool> alive_{true};
  std::atomic<std::thread::id> loop_thread_id_{};
};

}  // namespace dcb
