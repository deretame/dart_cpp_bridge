#pragma once

// ForeignExecutor — Generic external-runtime executor.
//
// Forwards async_simple::Executor::schedule() to a user-registered C callback.
// Any event loop only needs to implement "execute a void(*)(void*) on the loop thread"
// to plug into the bridge's channel / coroutine system.
//
// No dependency on asio. Only async_simple/Executor.h is required.
// See docs/foreign_runtime_design.md for details.

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

  // ─── async_simple::Executor interface ─────────────────────────────────────

  /// Post a task to the external runtime.
  /// Boxes the std::function on the heap and passes it via the C function pointer.
  /// The external loop thread frees and invokes the original function inside the trampoline.
  bool schedule(Func func) override {
    if (!alive_.load(std::memory_order_acquire)) return false;
    auto* boxed = new Func(std::move(func));
    schedule_fn_(&trampoline, boxed, ctx_);
    return true;
  }

  bool schedule(Func func, uint64_t /*schedule_info*/) override {
    return schedule(std::move(func));
  }

  /// Return whether the current thread is the external loop thread.
  /// async_simple calls this internally in many places (syncAwait assertion, FutureState scheduling, etc.).
  /// The base class default throws, so this must be overridden.
  bool currentThreadInExecutor() const override {
    auto id = loop_thread_id_.load(std::memory_order_acquire);
    return id != std::thread::id{} && std::this_thread::get_id() == id;
  }

  async_simple::ExecutorStat stat() const override {
    async_simple::ExecutorStat s;
    return s;
  }

  async_simple::IOExecutor* getIOExecutor() override { return nullptr; }

  // ─── ForeignExecutor-specific interface ─────────────────────────────────────

  const std::string& foreign_name() const { return name_; }
  bool alive() const { return alive_.load(std::memory_order_acquire); }

  /// Mark as inactive (called on unregister). Subsequent schedule calls return false.
  void deactivate() { alive_.store(false, std::memory_order_release); }

  /// Set the external loop thread ID (call after the loop thread starts).
  /// Enables currentThreadInExecutor() to work correctly.
  void set_loop_thread_id(std::thread::id id) {
    loop_thread_id_.store(id, std::memory_order_release);
  }

 private:
  /// Trampoline: runs on the external loop thread, frees the heap-allocated Func and invokes it.
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
