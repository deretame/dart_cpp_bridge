---
title: Choosing a Marker (v2)
description: How to choose BRIDGE_SYNC, BRIDGE_ASYNC, BRIDGE_NORMAL, streams, and DartFn in v2
---

The bridge uses `BRIDGE_*` markers to select dispatch behavior. The marker
names and Dart-facing return types are stable across v1 and v2; only the C++
async return type changes.

| Marker | Execution context | C++ signature in v2 | Dart return | Blocking |
| --- | --- | --- | --- | --- |
| `BRIDGE_SYNC` | bridge io thread | `T` | `T` | No |
| `BRIDGE_ASYNC` | io scheduler, suspending task | `stdexec::task<T>` | `Future<T>` | No |
| `BRIDGE_NORMAL` | blocking thread pool | `T` | `Future<T>` | Yes |
| Stream | selected by marker + `StreamSink<T>` | usually `void` or `stdexec::task<void>` | `Stream<T>` | Depends |

## `BRIDGE_SYNC`

Use it for short, non-blocking work:

```cpp
BRIDGE_SYNC
std::int32_t bridge_version() { return 42; }
```

Do not perform file or network I/O, sleep, acquire a potentially blocking lock,
or call DartFn from a sync function. The io thread must return promptly.

## `BRIDGE_ASYNC`

Use it for asynchronous I/O, timers, channels, and DartFn calls that can suspend:

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> fetch_name(std::string id) {
  auto value = co_await dcb::fetch_from_channel(id);
  co_return value;
}
```

A task may suspend without holding an io thread. Move genuinely blocking work
to `dcb::spawn_blocking` or use `BRIDGE_NORMAL`.

## `BRIDGE_NORMAL`

Use it for ordinary functions that may block:

```cpp
BRIDGE_NORMAL
std::string read_file(std::string path) {
  return read_file_synchronously(path);
}
```

The generated dispatch runs the function on the blocking pool and completes a
Dart `Future<String>`.

## Streams

A function is generated as a Dart stream when it has an export marker and a
required `dcb::StreamSink<T>` parameter. An optional
`std::optional<dcb::StreamSink<T>>` can be used on sync, async, or normal
functions when the stream is optional.

```cpp
BRIDGE_NORMAL
void ticks(dcb::StreamSink<std::int32_t> sink, std::int32_t count) {
  for (std::int32_t i = 0; i < count; ++i) {
    sink.add(i);
  }
  sink.end();
}
```

Stream unsubscription stops Dart delivery; the C++ operation may continue and
late sink calls are silently dropped.

## DartFn

DartFn is asynchronous in v2 because it returns a sender:

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> greet_dart(
    dcb::DartFn<std::string(std::string)> callback) {
  co_return co_await callback("from C++");
}
```

Do not call a potentially suspending DartFn from `BRIDGE_SYNC`. If a blocking
caller needs the result, use `dcb::sync_wait` off the io scheduler runners.

## Decision guide

1. Can the function finish quickly without blocking? Use `BRIDGE_SYNC`.
2. Does it need to suspend on a timer, channel, DartFn, or async I/O? Use
   `BRIDGE_ASYNC` and return `stdexec::task<T>`.
3. Does it perform blocking work? Use `BRIDGE_NORMAL`, or offload the blocking
   part with `dcb::spawn_blocking`.
4. Does it push values over time? Add the required `StreamSink<T>` and an export
   marker.

See [v2 stdexec Async C++](/dart_cpp_bridge/guides/fundamentals/stdexec/) and
[Exceptions and Error Handling](/dart_cpp_bridge/guides/fundamentals/errors/).
