# 外部运行时集成设计 (Foreign Runtime Integration)

> 状态：设计阶段  
> 目标：让非 asio 的事件循环（libuv、glib、自定义 loop 等）也能通过 channel 与 bridge 主运行时进行非阻塞消息传递。

## 1. 问题分析

### 当前架构

`dart_cpp_bridge` 的跨运行时通信依赖两个组件：

1. **channel**（`channel.hpp`）：`co::oneshot` / `co::mpsc` 协程 channel
2. **Executor**（`async_simple::Executor` 接口）：用于在 `send()` 时唤醒等待方的协程

通信链路：

```
channel send()
  → wake_waiter(coroutine_handle, executor)
    → executor->schedule([h]{ h.resume(); })
      → 协程在目标运行时的线程上恢复
```

### 耦合点

`AsioExecutor` 是目前唯一的 `Executor` 实现：

```cpp
class AsioExecutor : public async_simple::Executor {
  bool schedule(Func func) override {
    asio::post(ioc_, std::move(func));  // ← 强依赖 asio::io_context
    return true;
  }
  // ...
};
```

这意味着：
- 每个参与 channel 通信的运行时**必须**有 `asio::io_context`
- 每个运行时**必须**有 `async_simple::Executor` 实现
- asio 和 async-simple **缺一不可**

### 问题

如果另一个运行时使用 libuv（或 glib、IOCP、自定义 loop），它：
1. 无法提供 `async_simple::Executor` 实现（需要 C++ 继承 + async-simple 头文件）
2. 无法在自己的事件循环上恢复协程（channel 不知道如何 schedule 到 libuv）
3. 无法使用 `WorkerRuntime`（那个类硬编码了 `asio::io_context` + `AsioExecutor`）

## 2. 设计方案

### 核心思路

引入 **ForeignExecutor** 适配层：将 `async_simple::Executor::schedule()` 转发到一个 **C 函数指针回调**。
外部运行时只需实现一个 C 函数："在我的 loop 线程上执行这个 `void(*)(void*)`"。

### 层次结构

```
┌─────────────────────────────────────────────────────────────────┐
│  C API (foreign_runtime.h)                                       │
│  dcb_foreign_register / dcb_foreign_unregister                   │
│  dcb_post_to_bridge / dcb_foreign_executor                       │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│  ForeignExecutor (foreign_executor.hpp)                          │
│  implements async_simple::Executor                               │
│  schedule(fn) → heap-box fn → 调用 C 回调 → 外部 loop 唤醒执行   │
└──────────────────────────┬──────────────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────────────┐
│  具体运行时适配 (用户代码)                                        │
│                                                                   │
│  libuv:   uv_async_send 唤醒 → 从队列取 fn → 在 loop 线程执行    │
│  glib:    g_main_context_invoke → 在 main loop 线程执行           │
│  IOCP:    PostQueuedCompletionStatus → 工作线程执行               │
│  自定义:  条件变量 / eventfd / pipe 唤醒 → 执行                  │
└─────────────────────────────────────────────────────────────────┘
```

### 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 接口层级 | C 函数指针 | 任何语言/运行时都能实现，不暴露 C++ 类型 |
| Func 传递 | heap-box + trampoline | `std::function` 无法穿越 C 边界，需要装箱 |
| 生命周期 | `alive_` 原子标记 | 注销后 schedule 变为 no-op，防止 UAF |
| 双向通信 | `dcb_post_to_bridge` | 外部运行时也能主动向 bridge 投递任务 |
| 依赖隔离 | ForeignExecutor 不依赖 asio | 只需 `async_simple/Executor.h` 头文件 |

## 3. C API 设计

文件：`dart/native/include/dart_cpp_bridge/foreign_runtime.h`

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

// DCB_API 宏定义同 ffi.h（dllexport / visibility）

#ifdef __cplusplus
extern "C" {
#endif

/// 调度回调类型。
/// bridge 通过此回调向外部运行时投递一个任务。
/// 实现者必须保证 fn(userdata) 在目标事件循环的线程上被调用。
///
/// 参数：
///   fn      - 要执行的函数（通常是协程恢复）
///   userdata - fn 的参数（heap-allocated，fn 执行后由 fn 内部释放）
///   ctx      - 注册时传入的上下文指针
typedef void (*dcb_schedule_fn)(void (*fn)(void*), void* userdata, void* ctx);

/// 注册一个外部运行时。
///
/// 参数：
///   name        - 运行时名称（调试用，如 "libuv-worker"）
///   schedule_fn - bridge 用来向该运行时投递任务的回调
///   ctx         - 传给 schedule_fn 的上下文（如 UvWorker* 或 uv_loop_t*）
///
/// 返回：runtime_id (>0)，用于后续获取 executor 或注销。
///        返回 0 表示失败。
uint32_t dcb_foreign_register(const char* name, dcb_schedule_fn schedule_fn, void* ctx);

/// 可选：带原生定时器支持的注册（不破坏旧 API）。
/// schedule_after_fn 在 delay_us 微秒后在 loop 线程执行 fn(userdata)，
/// 返回不透明 timer 句柄（NULL=失败，bridge 回退等待线程）。
/// cancel_after_fn 取消挂起的定时器：必须线程安全，对已触发/未知句柄是 no-op。
/// 两个回调必须成对提供（可都传 NULL，等价于 dcb_foreign_register）。
uint32_t dcb_foreign_register_ex(
    const char* name,
    dcb_schedule_fn schedule_fn,
    dcb_schedule_after_fn schedule_after_fn,
    dcb_cancel_after_fn cancel_after_fn,
    void* ctx);

/// 注销外部运行时。
/// 注销后，任何对该运行时的 schedule 调用变为 no-op（安全降级）。
/// 已挂起的协程不会被恢复（调用方应确保 channel 已关闭或不再等待）。
void dcb_foreign_unregister(uint32_t runtime_id);

/// 从外部运行时向 bridge 主 io_context 投递一个任务。
/// 线程安全，非阻塞。任务在 bridge 的 io 线程上执行。
///
/// 典型用途：外部运行时处理完请求后，通过此函数将结果发回 bridge。
void dcb_post_to_bridge(void (*fn)(void*), void* userdata);

/// 获取外部运行时对应的 ForeignExecutor 指针。
/// 返回的指针可直接用于：
///   - channel 的 coAwait(executor) 路径
///   - Lazy.via(executor) 绑定
///   - 任何需要 async_simple::Executor* 的地方
///
/// 指针生命周期与 runtime_id 绑定，unregister 后不可再使用。
/// 返回 nullptr 表示 runtime_id 无效。
void* dcb_foreign_executor(uint32_t runtime_id);

#ifdef __cplusplus
}
#endif
```

### 3.1 可选：原生定时器回调

基础 C API 只会“投递任务”，不会“延时投递”，所以早期实现里
`co_await async_simple::coro::sleep(...)` 只能靠基类/回退的“每 sleep 开一个
等待线程”实现。为了让 libuv/glib 这类 loop 用上自己的定时器，新增一对可选
回调（见上方 `dcb_foreign_register_ex`）：

- `dcb_schedule_after_fn`：延时投递，返回不透明句柄；失败返回 NULL 时
  bridge 保证不调用 fn 且不使用 userdata，并回退到等待线程。
- `dcb_cancel_after_fn`：取消定时器，可被任意线程调用；实现方需要把操作
  marshal 到自己的 loop 线程（libuv 的 timer 句柄不能跨线程操作）。

`ForeignExecutor::schedule(Func, Duration, Slot*)` 优先走原生定时器；取消时
`Terminate` 处理器负责“停 timer + 把协程恢复投递回 loop”，并通过共享 job 的
原子标记保证恢复**恰好执行一次**（timer 正常触发与取消竞争时也不会双重
resume）。没有注册或启动失败时回退到等待线程，两条路径都支持取消。

## 4. ForeignExecutor C++ 类

文件：`dart/native/include/dart_cpp_bridge/foreign_executor.hpp`

```cpp
#pragma once

#include <async_simple/Executor.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace dcb {

/// 通用外部运行时执行器。
///
/// 将 async_simple::Executor::schedule() 转发到用户注册的 C 回调。
/// 任何事件循环只需实现 "在 loop 线程上执行一个 void(*)(void*)" 即可接入
/// bridge 的 channel / coroutine 系统。
///
/// 不依赖 asio。仅需要 async_simple/Executor.h。
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

  bool schedule(Func func) override {
    if (!alive_.load(std::memory_order_acquire)) return false;
    // 将 std::function 装箱到堆上，通过 C 函数指针传递
    auto* boxed = new Func(std::move(func));
    schedule_fn_(&trampoline, boxed, ctx_);
    return true;
  }

  bool schedule(Func func, uint64_t /*schedule_info*/) override {
    return schedule(std::move(func));
  }

  const std::string& name() const { return name_; }
  bool alive() const { return alive_.load(std::memory_order_acquire); }

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
};

}  // namespace dcb
```

### Trampoline 模式说明

问题：`async_simple::Executor::Func` 是 `std::function<void()>`，无法穿越 C 边界。

解决：
1. `schedule()` 将 `Func` 装箱到堆上（`new Func(...)`）
2. 传递 C 函数指针 `trampoline` + 装箱指针 `boxed` 给外部运行时
3. 外部运行时在 loop 线程上调用 `trampoline(boxed)`
4. `trampoline` 释放堆内存并执行原始函数

```
schedule(Func)
  → new Func on heap
  → schedule_fn_(trampoline, boxed, ctx)
    → [外部 loop 线程] trampoline(boxed)
      → unique_ptr 释放 + 执行 Func
```

## 5. libuv 完整适配示例

### 5.1 UvWorker 结构

```cpp
#include <uv.h>

#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <utility>

#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/foreign_runtime.h"

/// 基于 libuv 的独立运行时。
/// 演示如何通过 ForeignExecutor 接入 bridge 的 channel 系统。
class UvWorker {
 public:
  explicit UvWorker(std::string name) : name_(std::move(name)) {}

  ~UvWorker() { stop(); }

  void start() {
    if (running_) return;
    running_ = true;

    uv_loop_init(&loop_);

    // 初始化 async handle（用于跨线程唤醒 loop）
    uv_async_init(&loop_, &async_, [](uv_async_t* handle) {
      auto* self = static_cast<UvWorker*>(handle->data);
      self->drain_pending();
    });
    async_.data = this;

    // 注册到 bridge
    foreign_id_ = dcb_foreign_register(name_.c_str(), &schedule_callback, this);

    // 启动 loop 线程
    thread_ = std::thread([this] { uv_run(&loop_, UV_RUN_DEFAULT); });
  }

  void stop() {
    if (!running_) return;
    running_ = false;

    // 注销（之后 bridge 不再向我们 schedule）
    if (foreign_id_) {
      dcb_foreign_unregister(foreign_id_);
      foreign_id_ = 0;
    }

    // 停止 loop
    uv_stop(&loop_);
    // 唤醒 loop 使其退出 uv_run
    uv_async_send(&async_);

    if (thread_.joinable()) thread_.join();
    uv_loop_close(&loop_);
  }

  /// 获取 ForeignExecutor（用于 channel / Lazy.via()）
  dcb::ForeignExecutor* executor() {
    return static_cast<dcb::ForeignExecutor*>(dcb_foreign_executor(foreign_id_));
  }

  uint32_t foreign_id() const { return foreign_id_; }
  bool running() const { return running_; }

 private:
  /// bridge 调用此函数向我们投递任务（静态，C  linkage）
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
      fn(ud);  // 执行（如协程恢复）
    }
  }

  std::string name_;
  uv_loop_t loop_{};
  uv_async_t async_{};
  std::mutex mu_;
  std::queue<std::pair<void (*)(void*), void*>> pending_;
  std::thread thread_;
  uint32_t foreign_id_{0};
  bool running_{false};
};
```

### 5.2 业务逻辑（使用 channel 通信）

```cpp
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/runtime.hpp"

#include <async_simple/coro/Lazy.h>

// 示例：bridge 主运行时发请求到 libuv worker，worker 处理后回复
async_simple::coro::Lazy<std::string> ask_uv_worker(
    UvWorker& worker, std::string question) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  // 在 libuv loop 上执行（通过 ForeignExecutor）
  auto* ex = worker.executor();
  // 注意：这里不能用 WorkerRuntime::spawn（那是 asio 专用的）
  // 而是通过 ForeignExecutor::schedule 投递到 libuv loop
  ex->schedule([tx = std::move(tx), q = std::move(question)]() mutable {
    // 此代码在 libuv loop 线程上执行
    std::string answer = "[uv:" + q + "]";
    tx.send(std::move(answer));  // 非阻塞 send 回 bridge
  });

  // 在 bridge 主运行时上等待回复（挂起协程，不阻塞线程）
  auto reply = co_await rx.recv();
  co_return reply.value_or("uv worker dropped");
}
```

### 5.3 反向通信（libuv → bridge）

```cpp
// libuv worker 主动向 bridge 发送数据（如事件通知）
void uv_push_event(UvWorker& worker, std::string event) {
  // 从 libuv loop 线程调用，投递到 bridge 的 io_context
  dcb_post_to_bridge(
    [](void* p) {
      auto data = std::unique_ptr<std::string>(static_cast<std::string*>(p));
      // 在 bridge io 线程上执行
      // 例如：通过 mpsc channel 发送给正在等待的协程
      // g_event_tx.send(std::move(*data));
    },
    new std::string(std::move(event))
  );
}
```

## 6. 通信模式

### 完整请求/回复流程

```
┌────────┐         ┌──────────────────┐         ┌──────────────────┐
│  Dart  │         │  Main Runtime    │         │  libuv Worker    │
│        │         │  (asio io_ctx)   │         │  (uv_loop_t)     │
└───┬────┘         └────────┬─────────┘         └────────┬─────────┘
    │  invokeAsync          │                            │
    │──────────────────────>│                            │
    │                       │  oneshot::channel          │
    │                       │  tx.send → wake_waiter     │
    │                       │  → ForeignExecutor         │
    │                       │    ::schedule()            │
    │                       │  → schedule_callback()     │
    │                       │  → uv_async_send()         │
    │                       │───────────────────────────>│
    │                       │                            │ drain_pending()
    │                       │                            │ 执行协程/处理
    │                       │                            │ tx.send(reply)
    │                       │  wake_waiter               │
    │                       │  → AsioExecutor            │
    │                       │    ::schedule()            │
    │                       │<───────────────────────────│
    │                       │  co_await rx.recv() 恢复   │
    │  responseOk           │                            │
    │<──────────────────────│                            │
    │                       │                            │
```

### 与 multi_runtime_demo 的对比

| 方面 | multi_runtime_demo | foreign_runtime_demo |
|------|-------------------|---------------------|
| Worker 事件循环 | `asio::io_context` | `uv_loop_t` |
| Executor | `AsioExecutor` | `ForeignExecutor` |
| 唤醒机制 | `asio::post` | `uv_async_send` |
| 依赖 | asio + async-simple | libuv + async-simple（仅 Executor.h） |
| spawn 方式 | `WorkerRuntime::spawn` | `ForeignExecutor::schedule` |
| channel 通信 | 完全相同 | 完全相同 |

## 7. 文件规划

### 基础库层（`dart/native/`）

| 文件 | 内容 |
|------|------|
| `include/dart_cpp_bridge/foreign_runtime.h` | C API 声明（纯 C 头文件） |
| `include/dart_cpp_bridge/foreign_executor.hpp` | ForeignExecutor C++ 类 |
| `src/foreign_runtime.cpp` | C API 实现（注册表 + executor 生命周期管理） |

### 示例层（`examples/foreign_runtime_demo/`）

| 文件 | 内容 |
|------|------|
| `uv_worker.hpp` | UvWorker 类（libuv 适配） |
| `native/api/foreign_api.h` | BRIDGE_* 注解 API 声明 |
| `native/api_impl/foreign_api.cpp` | 业务逻辑（channel + libuv 通信） |
| `native/generated/` | codegen 生成的 wire dispatch |
| `lib/` | codegen 生成的 Dart 绑定 |
| `test/` | Dart 集成测试 |
| `CMakeLists.txt` | 构建配置（FetchContent libuv） |
| `dart_cpp_bridge.yaml` | codegen 配置 |

### CMake 依赖

```cmake
# libuv 通过 FetchContent 获取（仅示例需要）
include(FetchContent)
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG        v1.48.0
  GIT_SHALLOW    TRUE
)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libuv)
```

## 8. 实施步骤

1. **基础库实现**
   - 编写 `foreign_runtime.h`（C API 声明）
   - 编写 `foreign_executor.hpp`（ForeignExecutor 类）
   - 编写 `foreign_runtime.cpp`（注册表实现）
   - 在 `ffi_entry.cpp` 或独立文件中导出 C API

2. **示例实现**
   - 创建 `examples/foreign_runtime_demo/` 目录结构
   - 实现 `uv_worker.hpp`（libuv 适配）
   - 编写 `native/api/foreign_api.h`（BRIDGE_ASYNC 声明）
   - 编写 `native/api_impl/foreign_api.cpp`（业务逻辑）
   - 运行 codegen 生成绑定
   - 编写 Dart 测试

3. **验证**
   - C++ 构建通过
   - Dart 测试全绿
   - 验证非阻塞：libuv worker 处理期间 bridge io 线程不阻塞

## 9. 设计约束

1. **ForeignExecutor 不依赖 asio**：只 include `async_simple/Executor.h`，不引入任何 asio 头文件。
2. **C API 是纯 C**：不暴露 C++ 类型（std::function、std::string 等），任何语言/运行时都能调用。
3. **双向通信**：`dcb_post_to_bridge` 让外部运行时能主动向 bridge 发消息。
4. **安全注销**：unregister 后 schedule 变为 no-op（`alive_` 原子标记），防止 use-after-free。
5. **libuv 仅是示例**：基础库不链接 libuv，依赖通过示例的 CMake FetchContent 获取。
6. **不修改 channel.hpp**：channel 已经通过 `Executor*` 抽象解耦，无需改动。
7. **不修改 Runtime**：bridge 主运行时保持 asio 不变，ForeignExecutor 是纯增量。

## 10. 未来扩展

- **glib 适配**：`g_main_context_invoke(context, fn, userdata)` 即可实现 schedule_fn
- **Tokio/async-std (Rust)**：通过 FFI 暴露 `tokio::spawn` 作为 schedule_fn
- **多 ForeignExecutor 并行**：注册表支持多个外部运行时同时存在
- ~~定时器支持~~：**已实现**——`dcb_foreign_register_ex` + `dcb_schedule_after_fn` /
  `dcb_cancel_after_fn`，libuv demo 用 `uv_timer_t` 对接
- **线程池模式**：对于没有事件循环的场景，提供 `dcb_foreign_register_threadpool` 简化版
