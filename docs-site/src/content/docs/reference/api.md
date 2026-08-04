---
title: API Reference
description: Overview of public Dart and C++ APIs
---

## Dart API

### DartCppBridge

One instance per Isolate. Creates a Session and starts the process-wide Runtime on the first `init` call.

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

  // Low-level calls (used by codegen)
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

  // DartFn registration (used by codegen)
  int registerDartFn(Future<Uint8List> Function(Uint8List) fn);
  void unregisterDartFn(int id);
}
```

### CppOpaqueInterface

Dart base class for opaque C++ objects, providing `dispose()` and `NativeFinalizer` lifecycle management:

```dart
abstract base class CppOpaqueInterface implements Finalizable {
  int get handle;
  DartCppBridge get bridge;
  void dispose();
  void ensureAlive();
}
```

### Lifecycle

| Method | Description |
|---|---|
| `init()` | Initialize the current Isolate's Session and start the Runtime on demand |
| `dispose()` | Actively close the current Isolate's Session (optional; GC / Isolate exit will clean up automatically) |
| `shutdown()` | Close all Sessions and stop the Runtime; **only call from the main Isolate on process exit** |

:::caution
Do not call `shutdown()` in a worker isolate.
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

  // Must be called before start(); default is 4
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
`set_pool_threads()` must be called before `start()`. The Runtime thread pool size cannot be changed after it starts.
:::

| Function | Purpose | Description |
|---|---|---|
| `spawn(Lazy<T>)` | Returns `RescheduleLazy<T>` bound to the io executor | Not started; must call `start()` or `syncAwait()` |
| `spawn_detached(Lazy<T>)` | Fire-and-forget on the io thread | Results and exceptions are ignored |
| `spawn_blocking(F&&)` | Execute blocking / CPU tasks on the thread pool | Returns `Lazy<T>`; can be `co_await`ed in an io coroutine |
| `spawn_on_asio(LazyFactory)` | Schedule coroutines on the io_context thread | Used for low-level startup and cross-thread wake-up |

`syncAwait` usage example:

```cpp
// Safe: block and wait in the thread pool (BRIDGE_NORMAL) or an ordinary thread
auto r = async_simple::coro::syncAwait(dcb::spawn(compute_value()));
```

:::danger
Never call `syncAwait` on the `io_context` thread; it will cause a self-deadlock.
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

Required streams: give a `void` function an export marker (typically `BRIDGE_NORMAL`) and a
`StreamSink<T>` parameter — the generated Dart API returns `Stream<T>`. Optional streams:
receive `std::optional<StreamSink<T>>` in `BRIDGE_SYNC`, `BRIDGE_NORMAL`, or `BRIDGE_ASYNC`
functions — the generated Dart API takes a `StreamController<T>?` input parameter instead
(for `BRIDGE_SYNC`, events are delivered after the blocking FFI call returns). In both cases,
send stream data via `add()`, end the stream via `end()`, and report errors via `error()`.

### DartFn

```cpp
namespace dcb {

// Dart closures with arbitrary signatures
// DartFn<Ret(Args...)>
// Usage: co_await fn(args...) -> returns Lazy<Ret>
template <typename Ret, typename... Args>
class DartFn<Ret(Args...)> {
 public:
  async_simple::coro::Lazy<Ret> operator()(const Args&... args) const;
  explicit operator bool() const;
  std::uint64_t fn_id() const;
};

}  // namespace dcb
```

Blocking context call:

```cpp
auto reply = async_simple::coro::syncAwait(dcb::spawn(callback(arg)));
```

### Dispatch Registration

Hand-written or generated wire dispatch must be registered with the runtime before the first use:

```cpp
namespace dcb {

using DispatchRequestFn = void (*)(std::shared_ptr<Session>, std::uint64_t,
                                   const std::uint8_t*, std::size_t);
using DispatchSyncFn = std::vector<std::uint8_t> (*)(std::uint64_t,
                                                     const std::uint8_t*, std::size_t);

void set_dispatch(DispatchRequestFn async_fn, DispatchSyncFn sync_fn);

}  // namespace dcb
```

Usually done by file-scope static initialization in generated `wire_dispatch.cpp`; no manual call is needed.
