---
title: API 参考
description: Dart 和 C++ 公共 API 概览
---

## Dart API

### DartCppBridge

每个 Isolate 一个实例。首次调用 `init` 时创建 Session 并启动进程级 Runtime。

```dart
class DartCppBridge implements Finalizable {
  static DartCppBridge get instance;

  static Future<DartCppBridge> init({
    required NativeBindings bindings,
    int poolThreads = 4,
  });

  void dispose();
  void shutdown();
  void setVerboseErrors(bool enabled);

  // 底层调用（codegen 使用）
  Uint8List invokeSyncMethod(int methodId, [Uint8List? payload]);
  Future<Uint8List> invokeAsyncMethod(int methodId, [Uint8List? payload]);
  Future<Uint8List> invokeRawAsync(Uint8List rawBytes, {int? responseId});
  Stream<T> openStream<T>(int methodId, Uint8List payload, T Function(ByteReader) decodeItem);
  Future<Uint8List> invokeAsyncMethodWithStream<T>(
    int methodId,
    ByteWriter payload,
    StreamController<T>? controller,
    T Function(ByteReader) decodeItem,
  );

  // DartFn 注册（codegen 使用）
  int registerDartFn(Future<Uint8List> Function(Uint8List) fn);
  void unregisterDartFn(int id);
}
```

### CppOpaqueInterface

不透明 C++ 对象的 Dart 基类，提供 `dispose()` 和 `NativeFinalizer` 生命周期管理：

```dart
abstract base class CppOpaqueInterface implements Finalizable {
  int get handle;
  DartCppBridge get bridge;
  void dispose();
  void ensureAlive();
}
```

### 生命周期

| 方法 | 说明 |
|---|---|
| `init()` | 初始化当前 Isolate 的 Session，按需启动 Runtime |
| `dispose()` | 主动关闭当前 Isolate 的 Session（可选；GC / Isolate 退出时会自动清理） |
| `shutdown()` | 关闭所有 Session 并停止 Runtime；**只在主 Isolate 进程退出时调用** |

:::caution
`shutdown()` 不要在 worker isolate 中调用。
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
  bool running() const;

  // 必须在 start() 之前调用，默认 4
  void set_pool_threads(std::uint32_t n);

  asio::io_context& io();
  asio::thread_pool& pool();
  AsioExecutor* executor();

  void set_dart_post(DartPostFn fn, void* userdata);
  void post_to_dart(std::int64_t port, const std::uint8_t* data, std::size_t len);
};

}  // namespace dcb
```

:::caution
`set_pool_threads()` 必须在 `start()` 之前调用。Runtime 启动后线程池大小不可更改。
:::

| 函数 | 用途 | 说明 |
|---|---|---|
| `spawn(Lazy<T>)` | 返回绑定到 io executor 的 `RescheduleLazy<T>` | 未启动，需调用 `start()` 或 `syncAwait()` |
| `spawn_detached(Lazy<T>)` | 在 io 线程 fire-and-forget | 结果和异常都被忽略 |
| `spawn_blocking(F&&)` | 在线程池执行阻塞/CPU 任务 | 返回 `Lazy<T>`，可在 io 协程中 `co_await` |
| `spawn_on_asio(LazyFactory)` | 在 io_context 线程调度协程 | 用于底层启动和跨线程唤醒 |

`syncAwait` 使用示例：

```cpp
// 安全：在线程池（BRIDGE_NORMAL）或普通线程中阻塞等待
auto r = async_simple::coro::syncAwait(dcb::spawn(compute_value()));
```

:::danger
永远不要在 `io_context` 线程上调用 `syncAwait`，会自死锁。
:::

### StreamSink

```cpp
namespace dcb {

template <typename T>
class StreamSink {
 public:
  void add(const T& item);
  void end();
  void error(const std::string& message);
};

}  // namespace dcb
```

必需 stream：`void` 函数带导出标记（通常 `BRIDGE_NORMAL`）和 `StreamSink<T>` 参数，
生成的 Dart API 返回 `Stream<T>`。
可选 stream：在 `BRIDGE_SYNC`、`BRIDGE_NORMAL` 或 `BRIDGE_ASYNC` 函数中接收
`std::optional<StreamSink<T>>`，生成的 Dart API 变为 `StreamController<T>?` 输入参数
（`BRIDGE_SYNC` 的事件在阻塞 FFI 调用返回后送达）。两种方式都通过 `add()` 发送流数据、
`end()` 结束、`error()` 报错。

### DartFn

```cpp
namespace dcb {

// 任意签名的 Dart 闭包
// DartFn<Ret(Args...)>
// 调用方式：co_await fn(args...) → 返回 Lazy<Ret>
template <typename Ret, typename... Args>
class DartFn<Ret(Args...)> {
 public:
  async_simple::coro::Lazy<Ret> operator()(const Args&... args) const;
  explicit operator bool() const;
  std::uint64_t fn_id() const;
};

}  // namespace dcb
```

阻塞上下文调用：

```cpp
auto reply = async_simple::coro::syncAwait(dcb::spawn(callback(arg)));
```

### Dispatch 注册

手写或生成的 wire dispatch 需要在首次调用前注册到 runtime：

```cpp
namespace dcb {

using DispatchRequestFn = void (*)(std::shared_ptr<Session>, std::uint64_t,
                                   const std::uint8_t*, std::size_t);
using DispatchSyncFn = std::vector<std::uint8_t> (*)(std::uint64_t,
                                                     const std::uint8_t*, std::size_t);

void set_dispatch(DispatchRequestFn async_fn, DispatchSyncFn sync_fn);

}  // namespace dcb
```

通常由生成的 `wire_dispatch.cpp` 通过文件作用域静态初始化完成，无需手动调用。
