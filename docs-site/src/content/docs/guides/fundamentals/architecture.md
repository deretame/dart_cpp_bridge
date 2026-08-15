---
title: Architecture (v2)
description: dart_cpp_bridge layers, call flow, and the v2 stdexec runtime
---

:::caution[v2 development line]
The architecture below is the current stdexec implementation. The public Dart API,
C ABI, wire protocol, and generated file layout remain compatible with v1.
:::

## Layered architecture

```text
Dart / Flutter app
  └─ Dart package (DartCppBridge, codec, FFI bindings)
       └─ FFI binary frames
Native library
  ├─ Runtime (Asio io_context + IoContextScheduler + blocking pool)
  ├─ Session registry (one session per Dart isolate)
  ├─ Wire dispatch (frame routing and method_id)
  ├─ Codec (ByteReader / ByteWriter)
  ├─ Channels and stdexec scheduler adapters
  └─ User code (BRIDGE_SYNC / BRIDGE_ASYNC / BRIDGE_NORMAL)
```

The generator scans `BRIDGE_*` markers and emits the wire dispatch, Dart FFI
bindings, and the Dart API layer. Async C++ declarations use
`stdexec::task<T>` or another supported sender in v2.

## Runtime

The process-wide `dcb::Runtime~ owns:

- one Asio io thread exposed as `IoContextScheduler~;
- a blocking Asio thread pool;
- scheduler-aware channels and timers;
- the Dart post callback and session lifecycle.

A task started by generated dispatch is scheduler-affine. A foreign loop is not
wrapped as an async-simple executor in v2; it exposes a plain stdexec scheduler
that can be used with sender composition.

## Call flow

### Sync

```text
Dart → dcb_invokeSyncMethod
     → decode request on the io thread
     → call BRIDGE_SYNC function
     → encode responseOk / responseErr
     → return to Dart
```

The function must be short and non-blocking.

### Async

```text
Dart → dcb_invokeAsyncMethod
     → decode request
     → create stdexec::task<T>
     → starts_on(io_scheduler, task)
     → co_await sender / channel / timer / DartFn
     → post responseOk / responseErr to the Dart session
```

Generated coroutine dispatch uses a zero-capture IIFE. Arguments are passed as
parameters so the coroutine frame owns all state after the dispatch function
returns.

### Normal

```text
Dart → dcb_invokeNormalMethod
     → post ordinary C++ function to the blocking pool
     → encode result or exception
     → post response to Dart
```

### Stream and DartFn

A stream uses a `StreamSink<T>` to post `streamData`, `streamEnd`, or
`streamErr` frames. Dart unsubscription stops delivery; the native
operation may continue and late sink calls are dropped.

A `DartFn` call posts a `dartFnCall` frame and awaits a oneshot sender.
The io thread suspends while Dart executes the closure, then resumes when the
reply arrives.

## Threading model

- **io thread**: frame dispatch, scheduler work, non-blocking timers, and
  DartFn initiation;
- **blocking pool**: `BRIDGE_NORMAL` and `spawn_blocking` work;
- **foreign loop threads**: user-provided stdexec schedulers;
- **Dart isolate**: Dart code and closure execution.

Never block the io thread. Use `dcb::sync_wait` only from a worker or external
thread, and use stop tokens for cooperative cancellation.

## Further reading

- [v2 stdexec async C++](/dart_cpp_bridge/guides/fundamentals/stdexec/)
- [Built-in Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/)
- [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/)
- [Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/)
