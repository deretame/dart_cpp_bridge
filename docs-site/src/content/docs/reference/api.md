---
title: API 参考
description: Dart 和 C++ API 概览
---

## Dart API

### DartCppBridge

核心桥接类：

```dart
class DartCppBridge {
  // 初始化（每个 Isolate 调用一次）
  static Future<void> init();
  
  // 同步调用
  T invokeSyncMethod<T>(int methodId, Uint8List payload);
  
  // 异步调用
  Future<T> invokeAsyncMethod<T>(int methodId, Uint8List payload);
  
  // 关闭当前 Session
  void dispose();
  
  // 关闭所有 Session 并停止 Runtime
  static void shutdown();
}
```

### 生命周期

| 方法 | 说明 |
|---|---|
| `init()` | 创建 Session，启动 Runtime（如未启动） |
| `dispose()` | 关闭当前 Isolate 的 Session |
| `shutdown()` | 关闭所有 Session，停止 Runtime |

:::caution
`shutdown()` 只能从主 Isolate 在进程退出时调用，不要从 worker isolate 调用。
:::

## C++ API

### Runtime

```cpp
namespace dcb {

class Runtime {
 public:
  static Runtime& instance();
  
  void start();
  void stop();
  
  // 在 io_context 上调度协程
  void spawn_on_asio(async_simple::coro::Lazy<void> lazy);
  
  // 阻塞任务
  void spawn_blocking(std::function<void()> fn);
};

}  // namespace dcb
```

### StreamSink

```cpp
namespace dcb {

template <typename T>
class StreamSink {
 public:
  void add(T value);
  void close();
  void error(std::string message);
};

}  // namespace dcb
```

### spawn 家族

| 函数 | 用途 |
|---|---|
| `spawn_on_asio` | 在 io_context 线程调度协程 |
| `spawn_blocking` | 在线程池执行阻塞任务 |
| `spawn_normal` | 在线程池执行普通任务 |
