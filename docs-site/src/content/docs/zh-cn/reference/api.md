---
title: API 参考（v2）
description: 稳定 Dart API 与当前 v2 C++ Runtime API 概览
---

:::caution[v2 C++ 表面]
Dart API、C ABI 和 wire 协议在 v1/v2 中保持稳定。本页使用 v2 stdexec C++ 名称。
维护已发布的 1.x 项目时，请先查看[版本文档](/dart_cpp_bridge/zh-cn/versions/)。
:::

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

  Uint8List invokeSyncMethod(int methodId, [Uint8List? payload]);
  Future<Uint8List> invokeAsyncMethod(int methodId, [Uint8List? payload]);
  Stream<T> openStream<T>(
    int methodId,
    Uint8List payload,
    T Function(ByteReader) decodeItem,
  );

  int registerDartFn(Future<Uint8List> Function(Uint8List) fn);
  void unregisterDartFn(int id);
}
```

`shutdown()` 是进程级操作，只能由主 isolate 在进程退出时调用。
`dispose()` 关闭当前 isolate 的 session，native finalizer 也会执行清理。

### CppOpaqueInterface

生成的 opaque wrapper 使用 handle 和 `NativeFinalizer`：

```dart
abstract base class CppOpaqueInterface implements Finalizable {
  int get handle;
  DartCppBridge get bridge;
  void dispose();
  void ensureAlive();
}
```

## v2 C++ API

### Runtime

```cpp
namespace dcb {

class Runtime {
 public:
  static Runtime& instance();

  void start();
  void stop();
  bool running() const;
  void set_pool_threads(std::uint32_t n);

  DCB_ASIO_NS::io_context& io();
  IoContextScheduler* io_scheduler();
  auto blocking_scheduler();

  void set_dart_post(DartPostFn fn, void* userdata);
  void post_to_dart(std::int64_t port, const std::uint8_t* data, std::size_t len);
};

}  // namespace dcb
```

`io_scheduler()` 是单线程 Asio scheduler；blocking scheduler 用于
`BRIDGE_NORMAL` dispatch 和 `spawn_blocking`。

### Sender 与 task 辅助

```cpp
BRIDGE_ASYNC
stdexec::task<int> compute(int n);

auto sender = stdexec::starts_on(
    *dcb::Runtime::instance().io_scheduler(),
    stdexec::just(42));

auto result = dcb::sync_wait(std::move(sender));
```

只在 io 线程之外使用 `dcb::sync_wait`。fire-and-forget 应使用由 scope
持有的 stdexec spawn，并在销毁 scope 前排空它。

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

必需的 `StreamSink<T>` 参数加导出标记会生成 Dart `Stream<T>`。
可选 stream 可在 sync、async、normal 函数中使用
`std::optional<StreamSink<T>>`。

### DartFn

```cpp
namespace dcb {

template <typename Ret, typename... Args>
class DartFn<Ret(Args...)> {
 public:
  stdexec::sender auto operator()(const Args&... args) const;
  explicit operator bool() const;
  std::uint64_t fn_id() const;
};

}  // namespace dcb
```

`DartFn::operator()` 返回 sender。在 `stdexec::task` 中使用
`co_await callback(args...)`。阻塞调用方应在 worker 或外部线程使用
`dcb::sync_wait`。

### Dispatch 注册

生成的 `wire_dispatch.cpp~ 会通过 Runtime 内部注册机制注册 dispatch，通常
不需要应用手动调用。

完整 C ABI 和 wire frame 布局见 [Wire 协议](/dart_cpp_bridge/zh-cn/reference/wire-protocol/)。
