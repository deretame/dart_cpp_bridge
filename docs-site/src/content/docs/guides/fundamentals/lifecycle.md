---
title: Lifecycle Management
description: Lifecycle of Runtime, Session, Opaque objects, and NativeFinalizer
---

:::caution[v2 development line]
The runtime scheduler in this page is `IoContextScheduler`; `AsioExecutor` is a
v1 name. Session, Dart, and finalizer lifecycle rules are shared by both versions.
:::

The bridge lifecycle has three layers: process-level Runtime, Isolate-level Session, and object-level Opaque handle. Understanding who creates, who releases, and who must not be touched is key to avoiding deadlocks and memory leaks.

## Runtime: Process-Level Singleton

`dcb::Runtime` is a process-level singleton that internally manages:

- `asio::io_context` event loop
- `IoContextScheduler` (stdexec scheduler)
- `thread_pool`
- Session registry

### Startup and Shutdown

- **Startup**: usually triggered automatically by `DartCppBridge.init()`; C++ unit tests or pure C++ programs can call `dcb::Runtime::instance().start()` manually.
- **Shutdown**: `DartCppBridge.shutdown()` or `dcb::Runtime::instance().stop()`

**Limitation**: `shutdown()` can only be called from the **main isolate / on process exit**. It closes all Sessions and stops the Runtime.

## Session: One per Isolate

Every Dart Isolate that calls `DartCppBridge.init()` owns a Session:

- Independent reply port
- Independent `DartFn` closure registry
- Generation counter `generation`, used to drop late messages after `dispose()`

### Session Creation and Closing

| Operation | Caller Location | Behavior |
|---|---|---|
| `init()` | Any Isolate | Creates or reuses a Session |
| `dispose()` | Current Isolate | Immediately closes that Isolate's Session |
| Isolate shutdown / GC | Any | `NativeFinalizer` automatically closes the Session |
| `shutdown()` | Main isolate exit | Closes all Sessions |

### Rules

- `dispose()` is optional; normally rely on `NativeFinalizer` alone.
- Do not call `shutdown()` from a worker isolate.
- Worker isolates can call `init()`, own their own Session, but must not call `shutdown()`.

## Opaque Objects: Per-Session Handle

C++ objects marked with `BRIDGE_OPAQUE` are referenced from Dart via a handle:

- On construction: C++ creates the object, registers it in `ObjectHandleRegistry`, and returns a handle.
- On use: Dart passes the handle to C++ instance methods.
- On destruction: Dart GC triggers `NativeFinalizer` → `dcb_drop_object` → removes it from the registry and destructs it.

### Lifecycle Boundaries

- When a Session closes, all Opaque objects under that Session are automatically released.
- If Dart still holds an object reference but the Session is already closed, subsequent calls will fail.

## Typical Flow

```text
App starts
  └─ main isolate calls DartCppBridge.init()
       └─ Runtime starts (if not already started)
       └─ Session A is created
  ├─ worker isolate calls DartCppBridge.init()
  │    └─ Session B is created
  │
  ├─ Dart calls C++ to create an Opaque object
  │    └─ ObjectHandleRegistry registers it and returns a handle
  │
  ├─ Dart GC or dispose releases the Opaque object
  │    └─ NativeFinalizer → dcb_drop_object → destruct
  │
  └─ App exits
       └─ main isolate calls shutdown()
            └─ Sessions A / B close, Runtime stops
```

## Common Mistakes

| Mistake | Consequence |
|---|---|
| Worker isolate calls `shutdown()` | Closes the main isolate's Session, stops the Runtime, and breaks the bridge |
| Calling after `dispose()` | Calls from that isolate will fail |
| Holding Opaque objects across Isolates | Objects cannot be shared across Isolates |
| Calling DartFn inside `BRIDGE_SYNC` | Deadlock (Dart replies need the io thread) |

## Further Reading

- [Function Marker Selection Guide](/dart_cpp_bridge/guides/fundamentals/markers/)
- [Architecture Design](/dart_cpp_bridge/guides/fundamentals/architecture/)
