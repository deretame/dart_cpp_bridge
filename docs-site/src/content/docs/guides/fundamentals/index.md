---
title: Introduction
description: Project introduction and capability overview of dart_cpp_bridge
---

`dart_cpp_bridge` is a **Dart ↔ C++20** interoperability bridge library, inspired by [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/). It lets existing C/C++ code expose APIs to Dart/Flutter with an experience close to Dart `async` / `await` / `Stream`, while business C++ code only needs to be written as ordinary functions or `async_simple::coro::Lazy<T>` coroutines.

## What it can do

- **Synchronous calls**: `BRIDGE_SYNC` functions return directly; Dart-side calls are synchronous
- **Asynchronous coroutines**: `BRIDGE_ASYNC` functions return `Lazy<T>`, which can `co_await` suspend on the bridge io thread without blocking the thread
- **Stream flows**: C++ continuously pushes data to Dart via `StreamSink`
- **DartFn reverse calls**: C++ calls Dart closures (`Future`-style), supporting argument-based and persistent callbacks
- **C++ exception propagation**: C++ exceptions are caught at the wire boundary and encoded as Dart exceptions, without crashing the process
- **Code generation**: Parses `BRIDGE_*` markers in C++ headers and auto-generates Dart FFI bindings, wire dispatch, and serialization code
- **Cross-platform**: Supports Windows, Linux, macOS, iOS, and Android
- **Pure C API**: `cbridge.h` + `dcb_codec.h` provide C99-compatible callback-style entry points for pure C projects or other language runtimes
- **External runtime integration**: libuv, glib, and custom event loops can plug into the bridge coroutine system via `ForeignExecutor`
- **Coroutine channels**: `co::oneshot` / `co::mpsc` build non-blocking pipelines across threads / runtimes

## Design philosophy

Core principle:

> **Business C++ code is written as ordinary functions or `async_simple::coro::Lazy<T>`; the bridge layer handles encoding/decoding, scheduling, and Dart API generation.**

## Architecture overview

```text
Dart Isolate(s)
  Session per Isolate (one long-lived reply port)
  Future / Stream / DartFn callbacks
       ⇅  FFI binary frames
Runtime (process-wide)
  asio::io_context (single-threaded) + AsioExecutor
  asio::thread_pool (blocking / normal work)
  wire: sync / async Lazy / stream / DartFn
```

- **Runtime**: Process-wide singleton containing the `asio::io_context` event loop, `AsioExecutor`, and blocking thread pool
- **Session**: One Session per Isolate that calls `DartCppBridge.init()`, managing the reply port and DartFn closure registry
- **Wire**: Little-endian binary frames; C++ exceptions are caught at the wire boundary and encoded as error frames, never crossing FFI

## Built-in basic runtime

The bridge includes a built-in runtime based on **asio + async-simple**. Business code usually does not need to create its own event loop or executor. You can use it directly:

- `dcb::spawn` / `spawn_detached` / `spawn_blocking` — start coroutines and offload blocking tasks
- `co::oneshot` / `co::mpsc` — coroutine channels
- `async_simple::coro::sleep` — non-blocking timers (backed by `asio::steady_timer`)

See [Basic Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/) and [async-simple Coroutines Primer](/dart_cpp_bridge/guides/fundamentals/async-simple/) for details.

## Technology stack

| Layer | Technology | Notes |
|---|---|---|
| C++ standard | C++20 | Coroutines, concepts; requires a recent MSVC / GCC / Clang |
| Event loop | Asio standalone | Single-threaded `io_context` |
| Coroutines and channels | async-simple | `Lazy`, `Executor`; bridge provides `co::oneshot` / `co::mpsc` on top |
| Queue | moodycamel::ConcurrentQueue | Lock-free queue underlying `co::mpsc` |
| Dart side | Dart 3 + `package:ffi` | Isolates, `ReceivePort`, `Completer` / `Stream` |
| Code generation | Python 3.13 + libclang-ng | Parses C++ headers to generate bindings; toolchain version is locked |
| Build | CMake 3.24+ | FetchContent pulls Asio / async-simple / ConcurrentQueue |

## Next steps

- [Quick Start](/dart_cpp_bridge/getting-started/) — create a project, install tools, generate your first bindings
- [Architecture Design](/dart_cpp_bridge/guides/fundamentals/architecture/) — core components and call flow
- [Marker Selection Guide](/dart_cpp_bridge/guides/fundamentals/markers/) — choose `BRIDGE_SYNC` / `ASYNC` / `NORMAL` / `Stream` / `DartFn`
- [Streams](/dart_cpp_bridge/guides/fundamentals/streams/) — required and optional streams with `StreamSink`
- [Lifecycle Management](/dart_cpp_bridge/guides/fundamentals/lifecycle/) — Runtime, Session, Opaque objects, NativeFinalizer
- [Exceptions and Error Handling](/dart_cpp_bridge/guides/fundamentals/errors/) — C++ ↔ Dart exception propagation rules
- [Project Directory Structure](/dart_cpp_bridge/guides/fundamentals/project-structure/) — hand-written files and generated artifacts
- [Native Assets Build Hooks](/dart_cpp_bridge/guides/fundamentals/native-assets-hooks/) — how `hook/build.dart` compiles and bundles the C++ library
- [C++ ↔ Dart Type Encoding](/dart_cpp_bridge/guides/fundamentals/encoding/) — how C++ types map to Dart types
- [Basic Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/) — Runtime, spawn, channel, sleep
- [async-simple Coroutines Primer](/dart_cpp_bridge/guides/fundamentals/async-simple/) — `Lazy`, `Executor`, `co_await` behavior
- [Type Mapping](/dart_cpp_bridge/codegen/type-mapping/) — C++ ↔ Dart types and constraints
