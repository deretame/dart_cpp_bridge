# 多运行时通信示例 (multi_runtime_demo)

演示 `dart_cpp_bridge` 主调度器与多个独立 C++ 运行时之间如何通过协程 channel 进行**完全非阻塞**的消息传递。

## 架构总览

```text
┌─────────────────────────────────────────────────────────────────────┐
│  Dart Isolate                                                       │
│    await processMessage(message: "hello")                           │
│    workerStream(count: 5).listen(...)                               │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ FFI (wire frames)
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Main Runtime — dcb::Runtime                                        │
│  asio::io_context (单线程) + AsioExecutor                           │
│                                                                     │
│  职责: 接收 Dart 请求 → 通过 channel 分发 → 等待回复 → 回传 Dart     │
└───────┬─────────────────────────────────────────────────┬───────────┘
        │ co::oneshot / co::mpsc                          │
        │ (send 非阻塞, recv 挂起协程)                     │
        ▼                                                 ▼
┌───────────────────────────┐           ┌───────────────────────────┐
│  Worker A ("processor")   │           │  Worker B ("responder")   │
│  独立 asio::io_context    │           │  独立 asio::io_context    │
│  独立 AsioExecutor        │           │  独立 AsioExecutor        │
│  独立 std::thread         │           │  独立 std::thread         │
└───────────────────────────┘           └───────────────────────────┘
```

**核心原则**：每个运行时拥有自己的事件循环和线程，运行时之间**不共享线程**，**不互相阻塞**，唯一的通信方式是 channel。

## 通信机制：协程 Channel

本示例使用 `dart_cpp_bridge/channel.hpp` 提供的两种 channel：

### oneshot — 一次性请求/回复

```cpp
#include "dart_cpp_bridge/channel.hpp"

// 创建 channel（可跨线程使用）
auto [tx, rx] = co::oneshot::channel<std::string>();

// 发送端（任意线程，非阻塞）
tx.send("result");

// 接收端（协程内，挂起而非阻塞线程）
auto reply = co_await rx.recv();  // std::optional<std::string>
```

适用场景：一个请求对应一个回复（RPC 模式）。

### mpsc — 多生产者单消费者流

```cpp
auto [tx, rx] = co::mpsc::unbounded<std::string>();

// 发送端（可多次发送，非阻塞，返回 false 表示接收端已关闭）
tx.send("item_0");
tx.send("item_1");
// tx 析构 → channel 关闭 → recv 返回 nullopt

// 接收端（循环消费）
while (true) {
    auto item = co_await rx.recv();
    if (!item) break;  // channel 已关闭
    // 处理 *item
}
```

适用场景：持续数据流（Worker 产出多条数据 → Dart Stream）。

### 关键特性

| 特性 | 说明 |
|------|------|
| **send() 永不阻塞** | 可在任何线程调用，立即返回 |
| **recv() 挂起协程** | 不阻塞所在线程，仅暂停当前协程 |
| **线程安全** | 内部 mutex 保护，tx/rx 可跨线程移动 |
| **Executor 感知** | recv 被唤醒时通过 `ex->schedule()` 回到原运行时的线程恢复 |

## 演示的通信模式

### 1. 单 Worker 处理 (oneshot)

```
Dart → Main → Worker A 处理 → oneshot 回复 → Main → Dart
```

```cpp
// Main 侧: 创建 channel，分发任务到 Worker A
auto [tx, rx] = co::oneshot::channel<std::string>();
worker_a->spawn([tx = std::move(tx), msg]() mutable -> Lazy<> {
    tx.send("[A:" + msg + "]");  // Worker A 线程上执行
    co_return;
});
// Main 侧: 协程等待回复（不阻塞 Main 线程）
auto reply = co_await rx.recv();
```

### 2. Pipeline 链式处理 (oneshot 链)

```
Dart → Main → Worker A → Worker B → Main → Dart
```

两个 Worker 之间也通过 channel 通信，Worker B 的协程 `co_await` Worker A 的输出：

```cpp
auto [tx_ab, rx_ab] = co::oneshot::channel<std::string>();
auto [tx_final, rx_final] = co::oneshot::channel<std::string>();

// Worker A: 处理后发给 Worker B
worker_a->spawn([tx_ab]() mutable -> Lazy<> {
    tx_ab.send("A{data}");
    co_return;
});

// Worker B: 等待 A 的结果，再处理
worker_b->spawn([rx_ab, tx_final]() mutable -> Lazy<> {
    auto from_a = co_await rx_ab.recv();  // 挂起直到 A 发送
    tx_final.send("B[" + *from_a + "]");
    co_return;
});

// Main: 等待最终结果
auto result = co_await rx_final.recv();  // "B[A{data}]"
```

### 3. Fan-out 并行分发

```
         ┌→ Worker A → reply_a ─┐
Dart → Main                      ├→ Main 合并 → Dart
         └→ Worker B → reply_b ─┘
```

同时向两个 Worker 发送，两个 Worker 并行执行，Main 收集两个回复：

```cpp
auto [tx_a, rx_a] = co::oneshot::channel<std::string>();
auto [tx_b, rx_b] = co::oneshot::channel<std::string>();
worker_a->spawn([tx_a]() -> Lazy<> { tx_a.send("A:msg"); co_return; });
worker_b->spawn([tx_b]() -> Lazy<> { tx_b.send("B:msg"); co_return; });

auto a = co_await rx_a.recv();  // 两个 Worker 并行执行
auto b = co_await rx_b.recv();
```

### 4. Worker Stream (mpsc)

```
Worker A 持续产出 → mpsc channel → Main 消费 → Dart Stream
```

```cpp
auto [tx, rx] = co::mpsc::unbounded<std::string>();

// Worker A: 持续发送
worker_a->spawn([tx, count]() mutable -> Lazy<> {
    for (int i = 0; i < count; ++i) {
        tx.send("item_" + std::to_string(i));
    }
    // tx 析构 → channel 关闭
    co_return;
});

// Main: 消费并转发为 Dart stream 帧
while (true) {
    auto item = co_await rx.recv();
    if (!item) break;
    session->try_post(gen, make_frame(MsgType::kStreamData, ...));
}
session->try_post(gen, make_frame(MsgType::kStreamEnd, ...));
```

## WorkerRuntime 类

每个 Worker 是一个独立的运行时：

```cpp
class WorkerRuntime {
    asio::io_context ioc_;          // 独立事件循环
    dcb::AsioExecutor executor_;    // 独立协程调度器
    std::thread thread_;            // 独立线程

    void start();                   // 启动事件循环线程
    void stop();                    // 停止并 join

    // 在此 Worker 的事件循环上启动协程
    template <class LazyFactory>
    void spawn(LazyFactory&& factory);
};
```

`spawn()` 将协程投递到 Worker 自己的 `io_context`，协程内的 `co_await rx.recv()` 只挂起协程，不阻塞 Worker 线程。

## 为什么不会阻塞？

| 操作 | 行为 |
|------|------|
| `tx.send()` | 立即返回，将值放入 channel 内部队列，唤醒等待者 |
| `co_await rx.recv()` | 如果无数据，挂起当前协程（线程继续处理其他事件） |
| 唤醒 | sender 调用 `executor->schedule(resume)` 将协程恢复投递回接收方的事件循环 |
| Main 线程 | 永远不会被 Worker 阻塞；Worker 也永远不会被 Main 阻塞 |

## 构建与运行

```bash
# 前置: 先构建基础库（获取 asio / async-simple 依赖）
cmake -S ../../dart/native -B ../../dart/native/build
cmake --build ../../dart/native/build --config Release

# 运行 codegen 生成绑定（修改 native/api/*.h 后需重新执行）
cd ../../codegen
dart run bin/codegen.dart scripts/run_codegen.py ../examples/multi_runtime_demo/dart_cpp_bridge.yaml
cd ../examples/multi_runtime_demo

# 构建本 demo
cmake -S . -B build
cmake --build build --config Release

# 运行 Dart 测试
dart pub get
dart test
```

Windows 下如果自动检测 DLL 失败：

```powershell
$env:DCB_LIBRARY_PATH = "build\Release\dart_cpp_bridge.dll"
dart test
```

## 文件结构

```text
multi_runtime_demo/
├── dart_cpp_bridge.yaml           # codegen 配置
├── worker_runtime.hpp             # WorkerRuntime 类（独立事件循环）
├── native/
│   ├── api/
│   │   └── multi_runtime_api.h    # BRIDGE_* 注解头文件（API 定义）
│   ├── api_impl/
│   │   └── multi_runtime_api.cpp  # 业务实现（channel 通信逻辑）
│   └── generated/                 # ← codegen 自动生成，勿手动修改
│       ├── wire_dispatch.hpp
│       ├── wire_dispatch.cpp
│       └── ir.json
├── CMakeLists.txt                 # 构建配置
├── pubspec.yaml                   # Dart 包定义
├── lib/
│   ├── multi_runtime_demo.dart    # 包入口（export 生成代码）
│   └── src/native_gen/            # ← codegen 自动生成的 Dart 绑定
│       ├── gcm_generated.dart     #   BridgeApiImpl 单例
│       └── api/
│           ├── init.dart          #   DcbLib 初始化类
│           └── multi_runtime_api.dart  # 顶层函数 API
└── test/
    ├── multi_runtime_test.dart    # 13 个集成测试
    └── support/library_path.dart  # DLL 路径解析
```

## 设计要点

1. **运行时完全独立**：每个 Worker 拥有自己的 `io_context` + 线程，不共享任何调度状态。
2. **Channel 是唯一通信手段**：没有共享内存、没有回调、没有锁竞争（除了 Worker 生命周期的 mutex）。
3. **协程挂起 ≠ 线程阻塞**：`co_await` 只暂停协程，事件循环继续处理其他任务。
4. **Executor 感知恢复**：channel recv 被唤醒时通过 `schedule()` 回到原运行时的线程，避免跨线程恢复协程。
5. **与 Dart 无缝集成**：Dart 侧只看到 `Future<T>` 和 `Stream<T>`，底层的跨运行时通信完全透明。
6. **Codegen 自动生成绑定**：只需在 `native/api/*.h` 中用 `BRIDGE_ASYNC` / `StreamSink` 声明 API，codegen 自动生成 C++ wire dispatch 和 Dart 绑定。业务代码只写纯 `Lazy<T>` 协程。
