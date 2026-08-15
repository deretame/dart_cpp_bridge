---
title: API Reference (v2)
description: Overview of the stable Dart API and current v2 C++ runtime APIs
---

:::caution[v2 C++ surface]
The Dart API, C ABI, and wire protocol remain stable across v1 and v2. This page
uses the v2 stdexec C++ names. See [Versioned Documentation](/dart_cpp_bridge/versions/)
when maintaining a published 1.x application.
:::

## Dart API

### DartCppBridge

One instance per Isolate. The first `init` creates a Session and starts the
process-wide Runtime.

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

`shutdown()` is process-wide and must only be called by the main isolate
during process exit. `dispose()` closes the current isolate's session; the
native finalizer also performs this cleanup.

### CppOpaqueInterface

Generated opaque wrappers use a handle and `NativeFinalizer`:

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

`io_scheduler()` is the single-threaded Asio scheduler. The blocking
scheduler is used by `BRIDGE_NORMAL` dispatch and `spawn_blocking`.

### Sender and task helpers

```cpp
BRIDGE_ASYNC
stdexec::task<int> compute(int n);

auto sender = stdexec::starts_on(
    *dcb::Runtime::instance().io_scheduler(),
    stdexec::just(42));

auto result = dcb::sync_wait(std::move(sender));
```

Use `dcb::sync_wait` only off the io thread. For fire-and-forget work, use a
scope-owned stdexec spawn operation and drain the scope before destroying it.

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

A required `StreamSink<T>` parameter plus an export marker generates a Dart
`Stream<T>`. `std::optional<StreamSink<T>>` is supported for sync, async,
and normal functions when the stream is optional.

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

`DartFn::operator()` returns a sender. In a `stdexec::task`, use
`co_await callback(args...)`. A blocking caller must use `dcb::sync_wait`
from a worker or external thread.

### Dispatch registration

Generated `wire_dispatch.cpp~ registers the dispatch functions through the
runtime's internal registration hook. Application code normally does not call
this manually.

For the complete C ABI and wire frame layout, see [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/).
