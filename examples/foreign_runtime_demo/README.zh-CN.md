# Foreign Runtime Demo — 外部运行时集成

演示如何将非 asio 事件循环（本例为 libuv）接入 `dart_cpp_bridge` 的 channel/coroutine 系统，实现跨运行时非阻塞双向通信。

## 核心问题

bridge 的 `co::oneshot` / `co::mpsc` channel 依赖 `async_simple::Executor` 来唤醒等待方协程。
唯一内置实现 `AsioExecutor` 强耦合 `asio::io_context`，外部事件循环无法直接参与。

## 解决思路

引入 **ForeignExecutor** 适配层 + **C API 注册机制**：

```
┌────────────────────────────────────────────────────┐
│  外部运行时（libuv / glib / 自定义 loop）           │
│  只需实现：在 loop 线程上执行 void(*)(void*)        │
└──────────────────────┬─────────────────────────────┘
                       │ dcb_foreign_register(name, schedule_fn, ctx)
                       ▼
┌────────────────────────────────────────────────────┐
│  ForeignExecutor (async_simple::Executor 实现)      │
│  schedule(Func) → 装箱到堆 → 调用 schedule_fn      │
└──────────────────────┬─────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────┐
│  bridge channel 系统 (co::oneshot / co::mpsc)       │
│  send() → wake_waiter → executor->schedule(resume) │
└────────────────────────────────────────────────────┘
```

## 接入三步

### 1. 实现 schedule 回调

你的事件循环只需提供一个函数，将 `void(*)(void*)` 投递到 loop 线程执行：

```cpp
// libuv 示例
static void schedule_callback(void (*fn)(void*), void* userdata, void* ctx) {
  auto* self = static_cast<UvWorker*>(ctx);
  self->push_task({fn, userdata});   // 加锁入队
  uv_async_send(&self->async_);      // 唤醒 loop
}
```

### 2. 注册到 bridge

```cpp
uint32_t id = dcb_foreign_register("my-worker", &schedule_callback, this);
```

返回的 `id` 用于后续获取 executor 或注销。

### 3. 通过 channel 双向通信

**Bridge → 外部运行时**（bridge 侧发起请求，外部运行时处理并回复）：

```cpp
auto [tx, rx] = co::oneshot::channel<std::string>();
auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

// 投递到外部 loop 线程执行
executor()->schedule([tx_ptr, msg]() {
  std::string result = process(msg);   // 在 loop 线程上执行
  tx_ptr->send(std::move(result));     // 非阻塞回复，唤醒 bridge 协程
});

auto reply = co_await rx.recv();  // bridge 侧挂起等待（不阻塞 io 线程）
```

**外部运行时 → Bridge**（外部运行时主动推送）：

```cpp
// 在外部 loop 线程上
dcb_post_to_bridge(foreign_id_, [](void* ud) {
  // 此代码在 bridge 的 asio io_context 线程上执行
  auto* data = static_cast<MyData*>(ud);
  // ... 操作 bridge 侧资源
}, data);
```

## 关键设计约束

| 约束 | 说明 |
|------|------|
| schedule 必须线程安全 | bridge 可能从任意线程调用 |
| fn(userdata) 必须在 loop 线程执行 | 这是协程正确恢复的前提 |
| 不要阻塞 loop 线程 | sleep / 同步 IO 会卡死整个事件循环 |
| std::function 要求可拷贝 | 捕获 move-only 类型时用 `shared_ptr` 包装 |
| 注销后不再收到 schedule | `dcb_foreign_unregister` 后 executor 失效 |

## 本 demo 结构

```
examples/foreign_runtime_demo/
├── uv_worker.hpp              # libuv 适配器（uv_async_t + 任务队列）
├── native/
│   ├── api/foreign_api.h      # BRIDGE_ASYNC 注解声明
│   ├── api_impl/foreign_api.cpp  # 业务逻辑（channel 通信）
│   └── generated/             # codegen 生成（勿手动编辑）
├── lib/src/native_gen/        # codegen 生成的 Dart 绑定
├── test/foreign_runtime_test.dart  # 11 个测试
└── CMakeLists.txt             # FetchContent 拉取 libuv
```

## 构建 & 测试

```bash
# 1. 确保主库已构建（提供 asio/async-simple 依赖）
cmake -S ../../dart/native -B ../../dart/native/build
cmake --build ../../dart/native/build --config Release

# 2. 运行 codegen
cd ../../codegen
dart run bin/codegen.dart scripts/run_codegen.py ../examples/foreign_runtime_demo/dart_cpp_bridge.yaml

# 3. 构建本 demo
cd ../examples/foreign_runtime_demo
cmake -S . -B build
cmake --build build --config Release

# 4. 测试
dart pub get
dart test
```

## 适配其他运行时

只需替换 `uv_worker.hpp` 中的事件循环部分。例如 glib：

```cpp
static void schedule_callback(void (*fn)(void*), void* ud, void* ctx) {
  auto* self = static_cast<GlibWorker*>(ctx);
  // g_idle_add 在 main context 线程上执行
  g_idle_add([](gpointer p) -> gboolean {
    auto [f, u] = *static_cast<std::pair<void(*)(void*), void*>*>(p);
    f(u);
    return G_SOURCE_REMOVE;
  }, new std::pair{fn, ud});
}
```

核心不变：**实现 `void(*)(void*)` 在 loop 线程执行 → 注册 → channel 通信**。

## 从外部运行时调用 Dart 回调 (DartFn)

外部运行时通过 ForeignExecutor 接入后，可以在 loop 线程上调用已注册的 Dart 回调。提供两种方式：

### 非阻塞（推荐）

等待 Dart 回复期间 **不阻塞** loop 线程。协程挂起后，Dart 回复时自动在 loop 线程上恢复。

**前提条件：**
- ForeignExecutor 必须实现完整的 `async_simple::Executor` 接口（`currentThreadInExecutor()`、`stat()`、`getIOExecutor()`）
- loop 线程启动时调用 `dcb_foreign_mark_loop_thread(id)`
- MSVC 下：必须使用 static 协程函数，不能用协程 lambda（见 [known_issues.md §10](../../docs/known_issues.md)）

```cpp
// static 协程函数（MSVC workaround —— 不要用协程 lambda）
static async_simple::coro::Lazy<> my_dart_fn_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  try {
    auto result = co_await cb(input);  // 非阻塞：挂起直到 Dart 回复
    tx_ptr->send(std::move(result));
  } catch (const std::exception& e) {
    tx_ptr->send(std::string("ERROR: ") + e.what());
  }
  co_return;
}

// API 函数中（运行在 bridge io 线程）：
async_simple::coro::Lazy<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  auto* ex = worker->executor();
  auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

  // 在外部 loop 线程上启动协程
  ex->schedule([tx_ptr, cb = std::move(callback), input = std::move(input), ex]() mutable {
    my_dart_fn_coro(std::move(tx_ptr), std::move(cb), std::move(input))
        .via(ex)
        .start([](auto&&) {});
  });

  auto reply = co_await rx.recv();
  co_return *reply;
}
```

**工作原理：**
1. `ex->schedule(...)` 将协程启动投递到 loop 线程
2. `.via(ex).start()` 将协程绑定到 ForeignExecutor 并开始执行
3. `co_await cb(input)` 发送请求到 Dart 并挂起协程
4. Dart 回复时，`wake_waiter` 通过 ForeignExecutor 调度恢复
5. 协程在 loop 线程上恢复，获得结果

### 阻塞（更简单，但卡 loop）

阻塞当前线程直到 Dart 回复。可在任何线程使用（bridge io 线程除外 —— 会自死锁）。无需 ForeignExecutor 配置。

```cpp
// 在任意工作线程上（不能是 bridge io 线程！）：
auto result = async_simple::coro::syncAwait(dcb::spawn(cb(input)));
```

> **注意：** 在 loop 线程上使用 `syncAwait` 会阻塞整个事件循环直到 Dart 响应。仅适用于快速回调或 loop 无其他工作的场景。

### 对比

| | 非阻塞 | 阻塞 |
|--|---|---|
| 是否卡 loop | 否 | 是（直到 Dart 回复） |
| ForeignExecutor 配置 | 完整（虚函数 + mark_loop_thread） | 无 |
| MSVC workaround | 需要（static 协程函数） | 不需要 |
| 适用场景 | 生产环境、并发工作 | 快速测试、简单脚本 |

---

## 性能与优化方向

### 当前实现的锁开销

| 位置 | 用途 | 临界区 |
|------|------|--------|
| `UvWorker::mu_` | 任务队列 push/swap | 纳秒级（仅 queue 操作） |
| `g_mu` | 保护 worker 指针 | 非热路径（仅 start/stop） |
| `ForeignExecutor` | 无锁 | `atomic<bool>` 检查 |

对于中低频消息（几千 msg/s），mutex 完全不是瓶颈。

### 高频场景优化（几十万 msg/s）

1. **Lock-free MPSC 队列**：替换 `mutex + std::queue` 为无锁链表（如 intrusive MPSC），消除 schedule 路径上的锁竞争。

2. **批量唤醒**：攒一批任务后再调用 `uv_async_send`，减少跨线程唤醒次数：
   ```cpp
   // 伪代码：每 N 个任务或每 T 微秒唤醒一次
   if (pending_count++ % BATCH_SIZE == 0) uv_async_send(&async_);
   ```

3. **避免 shared_ptr 开销**：当前用 `shared_ptr` 包装 Sender 是为了满足 `std::function` 可拷贝约束。若改用 move-only 的 `unique_function`（C++23 `std::move_only_function` 或自定义），可省掉引用计数和堆分配。

4. **零拷贝传输**：对于大 payload，可传递指针/引用而非拷贝值，配合 channel 的 move 语义。

### 设计取舍

本 demo 选择 mutex + `shared_ptr` 是因为：
- 正确性优先，代码清晰易懂
- 适配器的核心目标是演示接入模式
- 真正的瓶颈通常在业务逻辑（如 uv loop 里的 IO），不在队列锁

生产使用时，只需替换 `schedule_callback` 内部的队列实现，**C API 接口不变**。
