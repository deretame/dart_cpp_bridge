---
title: Streams
description: Expose C++ data streams to Dart with StreamSink, including optional streams
slug: v1/guides/fundamentals/streams
---

## Overview

A C++ function becomes a Dart `Stream<T>` when it has an export marker
(`BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL`) **and** takes a required
`dcb::StreamSink<T>` parameter. The export marker is the gate: a function with a `StreamSink`
parameter but no export marker is not generated (the generator warns and skips it). Plain
`void` stream functions typically use `BRIDGE_NORMAL`; the sink parameter overrides the
marker's usual scheduling and the generated Dart API returns `Stream<T>`.

| C++ | Dart |
|---|---|
| `void f(dcb::StreamSink<T> sink, ...)` | `Stream<T> f(...)` |
| `R f(args..., std::optional<dcb::StreamSink<T>> progress)` (`BRIDGE_SYNC`) | `R f(..., {StreamController<T>? progress})` |
| `Lazy<R> f(args..., std::optional<dcb::StreamSink<T>> progress)` | `Future<R> f(..., {StreamController<T>? progress})` |

The function may return immediately: the sink can be kept and used later from any thread.
This page uses the [codegen\_demo](https://github.com/deretame/dart_cpp_bridge/tree/main/examples/codegen_demo) fixture as a reference.

## Required stream

### C++ header

```cpp
BRIDGE_NORMAL
void tick_stream(dcb::StreamSink<std::int32_t> sink, std::int32_t count = 5,
                 std::int32_t interval_ms = 10);
```

### C++ implementation

The function is an ordinary `void` function. It usually returns immediately and the real work
continues on another thread (here the runtime thread pool); `sink.add()` is thread-safe.

```cpp
void tick_stream(dcb::StreamSink<std::int32_t> sink, std::int32_t count,
                 std::int32_t interval_ms) {
  asio::post(dcb::Runtime::instance().pool(),
             [sink = std::move(sink), count, interval_ms]() mutable {
               for (std::int32_t i = 0; i < count; ++i) {
                 sink.add(i);
                 if (interval_ms > 0) {
                   std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                 }
               }
               sink.end();
             });
}
```

### Generated Dart

```dart
Stream<int> tickStream({int count = 5, int intervalMs = 10}) => ...;
```

### Dart usage

```dart
// Collect all events:
final values = await tickStream(count: 5, intervalMs: 10).toList();
// values == [0, 1, 2, 3, 4]

// Or listen and cancel:
final sub = tickStream(count: 100, intervalMs: 10).listen((v) { ... });
await sub.cancel();
```

## Optional stream

`std::optional<dcb::StreamSink<T>>` changes the Dart signature from "returns a stream" to
"accepts a stream": the caller passes a `StreamController<T>?` input parameter. When `null`
is passed, C++ receives `std::nullopt`, no stream events flow, and the function still returns
its normal result. It works on `BRIDGE_SYNC`, `BRIDGE_ASYNC`, and `BRIDGE_NORMAL` functions.
For `BRIDGE_SYNC`, events posted by C++ during the blocking FFI call are queued on the reply
port and delivered to the controller right after the call returns.

### C++ header

```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress);
```

### C++ implementation

The coroutine runs on the io thread: emit events as part of the asynchronous
work (between `co_await` points) and never block the thread.

```cpp
async_simple::coro::Lazy<std::string> download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  for (std::int32_t i = 1; i <= 5; ++i) {
    if (progress) {
      progress->add(i * 20); // 20, 40, 60, 80, 100
      // In real code events come from async work; a cancellable sleep
      // simulates the async interval here.
      co_await async_simple::coro::sleep(std::chrono::milliseconds(10));
    }
  }
  co_return std::string("downloaded: ") + url;
}
```

### Generated Dart

```dart
Future<String> downloadWithProgress({
  required String url,
  StreamController<int>? progress,
}) => ...;
```

### Dart usage

```dart
// With progress events:
final controller = StreamController<int>();
controller.stream.listen(progressValues.add);
final result = await downloadWithProgress(
  url: 'https://example.com/file.zip',
  progress: controller,
);
await controller.close(); // close the controller yourself when you are done

// Without progress:
final result = await downloadWithProgress(url: 'test.txt');
```

The same request carries both the stream events and the final result: `streamData` /
`streamEnd` frames for the stream and a final `responseOk` frame for the returned value.

### Sync variant

`BRIDGE_SYNC` functions use the same `std::optional<dcb::StreamSink<T>>` parameter; the
generated Dart API is synchronous:

```cpp
BRIDGE_SYNC
std::string sync_download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress);
```

### C++ implementation

The sync function must not emit events inside the blocking FFI call — spawn a coroutine
that sends them asynchronously, then return the result immediately:

```cpp
namespace {
// Free coroutine function: parameters are copied into the coroutine frame,
// avoiding the dangling-capture problem of a coroutine lambda.
async_simple::coro::Lazy<> emit_progress(dcb::StreamSink<std::int32_t> sink) {
  for (std::int32_t i = 1; i <= 5; ++i) {
    sink.add(i * 20);  // 20, 40, 60, 80, 100
  }
  co_return;
}
}  // namespace

std::string sync_download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  if (progress) {
    dcb::spawn_detached(emit_progress(std::move(*progress)));
  }
  return std::string("downloaded: ") + url;
}
```

The events are queued on the reply port while the FFI call is still in flight and delivered
to the controller right after the call returns. `dcb::spawn_detached` is the fire-and-forget
launcher (`dcb::spawn(lazy).start([](auto&&) {})`); do not use a coroutine lambda here — its
captures would dangle once the temporary lambda object is destroyed.

```dart
final controller = StreamController<int>();
controller.stream.listen(progressValues.add);
final result = syncDownloadWithProgress(
  url: 'https://example.com/file.zip',
  progress: controller,
);
// result is available immediately; progress events arrive right after.
await Future<void>.delayed(const Duration(milliseconds: 20));
expect(progressValues, [20, 40, 60, 80, 100]);
await controller.close();
```

## Errors

* `sink.error(message)` sends an error event to the Dart stream and closes it — this is the
  supported way to signal a stream-level error.
* For a required stream, the business function usually returns immediately; do not rely on
  throwing from the dispatch path — use `sink.error()` instead.
* For an optional stream, a C++ exception fails the returned `Future`, but the bridge does
  not close the `StreamController` automatically. Call `progress->error(...)` in C++ or
  `controller.close()` in Dart to finish the stream.

## Cancellation

* Dart `sub.cancel()` sends `dcb_stream_close`; the native stream is marked closed.
* The C++ side is **not** interrupted: it keeps running, and later `add()` / `end()` calls are
  silently dropped.
* A new call creates a fresh stream with a new stream id, so cancel-then-resubscribe works
  (covered by the codegen\_demo stress tests).

## Threads and lifetime

* `add()` / `end()` / `error()` are thread-safe and can be called from the io thread, a pool
  thread, or a foreign event-loop thread (see `uv_stream` / `worker_stream` in codegen\_demo,
  which bridge an mpsc channel into the sink on the io thread).
* The sink holds a `std::shared_ptr<Session>` internally, so it may outlive the dispatch frame
  and even a session close; after the session is disposed, late calls are silently dropped by
  the generation check instead of touching freed memory.

## Opaque class member streams

Opaque classes can also expose streams as member functions:

```cpp
class BRIDGE_OPAQUE Counter {
 public:
  void tickStream(dcb::StreamSink<std::int32_t> sink, std::int32_t count = 5,
                  std::int32_t intervalMs = 10);
};
```

```dart
final counter = Counter.int32T(initialValue: 3);
final values = await counter.tickStream(count: 3, intervalMs: 10).toList();
// values == [3, 3, 3]
```

## Further reading

* [Choosing a Marker](/dart_cpp_bridge/v1/guides/fundamentals/markers/)
* [Basic Runtime](/dart_cpp_bridge/v1/guides/fundamentals/runtime/)
* [Exceptions and Error Handling](/dart_cpp_bridge/v1/guides/fundamentals/errors/)
* [Type Mapping](/dart_cpp_bridge/v1/codegen/type-mapping/)
