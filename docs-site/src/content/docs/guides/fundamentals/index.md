---
title: Fundamentals (v2)
description: Core concepts for the current stdexec-based dart_cpp_bridge implementation
---

:::caution[v2 development line]
The current guides describe the stdexec implementation on the migration branch.
For a released 1.x application, start with [Versioned Documentation](/dart_cpp_bridge/versions/)
and use the v1 archive pages.
:::

`dart_cpp_bridge` is a **Dart ↔ C++20** bridge inspired by
[Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/). Business code
is written as ordinary C++ functions or `stdexec::task<T>` / sender pipelines;
the bridge handles codec, scheduling, lifecycle, and Dart API generation.

## Core capabilities

- **Sync** — `BRIDGE_SYNC` returns a value directly.
- **Async** — `BRIDGE_ASYNC` returns `Future<T>` from a
  `stdexec::task<T>` or supported sender.
- **Normal** — `BRIDGE_NORMAL` runs blocking work on the thread pool.
- **Streams** — `StreamSink<T>` produces Dart `Stream<T>`.
- **DartFn** — C++ can suspend while awaiting a Dart closure reply.
- **Channels** — `co::oneshot` and `co::mpsc` connect workers and coroutines.
- **Opaque/data classes** — generated handles and value codecs.
- **Cross-runtime scheduling** — foreign event loops provide plain stdexec
  schedulers.

## Current stack

| Layer | Technology |
| --- | --- |
| C++ | C++20 |
| Async model | stdexec senders / `stdexec::task` |
| Built-in event loop | Asio `io_context`, one io thread |
| Blocking work | Asio thread pool |
| Cancellation | stop tokens |
| Codegen | Python 3.13 + libclang-ng, pinned and hash-verified |
| Build | CMake 3.24+ and Native Assets hooks |

## Read next

- [Versioned Documentation](/dart_cpp_bridge/versions/) — choose v1 or v2
- [Getting Started](/dart_cpp_bridge/getting-started/) — create a project
- [Architecture](/dart_cpp_bridge/guides/fundamentals/architecture/) — call flow
- [Choosing a Marker](/dart_cpp_bridge/guides/fundamentals/markers/) — select sync, async, normal, or stream
- [v2 stdexec Async C++](/dart_cpp_bridge/guides/fundamentals/stdexec/) — sender and task rules
- [Channels](/dart_cpp_bridge/guides/fundamentals/channels/) — oneshot, mpsc, backpressure, and cancellation
- [Threading and Blocking Work](/dart_cpp_bridge/guides/fundamentals/threading/) — pool sizing and custom schedulers
- [Built-in Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/) — Runtime and scheduler
- [Lifecycle Management](/dart_cpp_bridge/guides/fundamentals/lifecycle/) — sessions and finalizers
- [Native Assets Build Hooks](/dart_cpp_bridge/guides/fundamentals/native-assets-hooks/) — native build integration
