---
title: Marker Selection Guide
description: How to choose BRIDGE_SYNC / BRIDGE_ASYNC / BRIDGE_NORMAL / stream / DartFn
---

The bridge uses `BRIDGE_*` markers to decide how C++ functions are exposed to Dart. Choosing the wrong one can cause blocking, deadlocks, or performance issues. This page is a cheat sheet plus decision flowchart.

## Overview

| Marker | Execution Thread | C++ Return Type | Dart Return Type | Can Block | Can Call DartFn |
|---|---|---|---|---|---|
| `BRIDGE_SYNC` | io_context | `T` | `T` | ❌ | ❌ (deadlock) |
| `BRIDGE_ASYNC` | io_context coroutine | `async_simple::coro::Lazy<T>` | `Future<T>` | ❌ | ✅ (co_await) |
| `BRIDGE_NORMAL` | thread_pool | `T` | `Future<T>` | ✅ | ✅ (syncAwait) |
| Stream | io_context | `void` + `StreamSink<T>` | `Stream<T>` | ❌ | Depends on implementation |

Core principles:

- **Never block the io_context thread**
- **DartFn cannot be used with `BRIDGE_SYNC`**

## 1. BRIDGE_SYNC — Synchronous Calls

The C++ function executes synchronously on the bridge's io thread, and the result is returned to Dart immediately.

```cpp
BRIDGE_SYNC
std::int32_t bridge_version() { return 42; }
```

Suitable for:

- Pure computation, getters, constant reads
- Microsecond-level operations (usually < 1 μs)
- No file access, network, locks, or sleep

Not suitable for:

- Blocking calls
- Calling DartFn (permanent deadlock, because the Dart reply requires the io thread)

## 2. BRIDGE_ASYNC — Asynchronous Coroutines

The C++ function returns `async_simple::coro::Lazy<T>` and runs as a coroutine on the io thread; when it hits `co_await`, it suspends without occupying the thread.

```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b) {
  co_return a + b;
}

BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> fetch_url(std::string url) {
  // You can co_await channels, sleep, DartFn, or spawn_blocking.
  auto result = co_await co::oneshot::recv();
  co_return result;
}
```

Suitable for:

- Asynchronous IO
- Waiting for other coroutines / channels / Dart callbacks
- Composing multiple asynchronous operations

Not suitable for:

- Blocking operations (use `BRIDGE_NORMAL` or `spawn_blocking`)

## 3. BRIDGE_NORMAL — Normal Functions

The C++ function is an ordinary function (it does not return `Lazy`); the bridge automatically dispatches it to the `thread_pool`. On the Dart side it is still `Future<T>`.

```cpp
BRIDGE_NORMAL
std::string sleep_greeting(std::string name) {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return "Hello, " + name;
}
```

Suitable for:

- File IO, synchronous network IO libraries
- CPU-intensive computation
- Any operation that blocks or takes more than microseconds

Notes:

- The function can block internally because it runs on the thread pool, not the io thread
- You can still call DartFn via `async_simple::coro::syncAwait(dcb::spawn(...))`

## 4. Stream — Streams

Stream functions run on the io thread and push data to Dart via `StreamSink<T>`.

```cpp
BRIDGE_ASYNC
void ticks(dcb::StreamSink<std::int32_t> sink, std::int32_t count,
           std::int32_t interval_ms) {
  for (int i = 0; i < count; ++i) {
    sink.add(i);
    co_await async_simple::coro::sleep(std::chrono::milliseconds(interval_ms));
  }
  sink.end();
}
```

Notes:

- Cancelling a subscription only stops Dart-side reception; the C++ side continues to run
- `add()` calls after cancellation are silently dropped

## 5. DartFn — Reverse Dart Closure Calls

`dcb::DartFn<Ret(Args...)>` represents a Dart closure. It is not a function marker itself, but a parameter type, and must be paired with `BRIDGE_ASYNC` or `BRIDGE_NORMAL`.

### Asynchronous Calls (Recommended)

Call it with `co_await` inside a `BRIDGE_ASYNC` coroutine; the io thread actually suspends.

```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> greet(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  auto reply = co_await callback(name);
  co_return "Dart said: " + reply;
}
```

### Persistent Callbacks

Use the `BRIDGE_PERSIST` marker so the closure is not automatically unregistered after the call returns; it can be stored and invoked repeatedly.

```cpp
BRIDGE_SYNC
BRIDGE_PERSIST
bool register_callback(dcb::DartFn<std::string(std::string)> callback);

BRIDGE_NORMAL
std::string invoke_callback(std::string name);
```

### Prohibited

```cpp
// ❌ Deadlock
BRIDGE_SYNC
std::string bad(dcb::DartFn<std::string(std::string)> callback);
```

## Decision Flow

```text
Need to return a Stream?
  → Yes: BRIDGE_ASYNC with StreamSink<T>

Need to call a Dart closure?
  → Yes: BRIDGE_ASYNC + DartFn (co_await)
  → Or: BRIDGE_NORMAL + syncAwait(dcb::spawn(fn(args)))

Will the function block / take time / do file IO?
  → Yes: BRIDGE_NORMAL

Is the function asynchronous, needing co_await / channel / sleep?
  → Yes: BRIDGE_ASYNC

Is it just pure computation / getter / microsecond-level operation?
  → Yes: BRIDGE_SYNC
```

## Common Mistakes

| Mistake | Consequence |
|---|---|
| Blocking inside `BRIDGE_SYNC` | io thread stalls, the entire bridge becomes unresponsive |
| `BRIDGE_SYNC` + DartFn | Permanent deadlock |
| Blocking inside `BRIDGE_ASYNC` | Same as blocking inside `BRIDGE_SYNC` |
| Writing `co_await` inside `BRIDGE_NORMAL` | Compile error, because it is not a coroutine |

## Further Reading

- [async-simple Coroutine Primer](/dart_cpp_bridge/guides/fundamentals/async-simple/)
- [Basic Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/)
- [Exceptions and Error Handling](/dart_cpp_bridge/guides/fundamentals/errors/)
