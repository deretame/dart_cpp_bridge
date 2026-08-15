---
title: stdexec in v2
description: A practical guide to sender, task, scheduler, cancellation, and blocking work
---

:::caution[v2 only]
The current development line uses stdexec. Published v1 applications use
async-simple; their API is preserved in the [v1 documentation](/dart_cpp_bridge/v1/).
Do not mix `async_simple::coro::Lazy` examples into a v2 API header.
:::

## The mental model

stdexec separates an asynchronous operation into three values:

- a **sender** is a lazy recipe for an operation;
- a **scheduler** describes where work starts or continues;
- a **receiver** consumes exactly one completion: value, error, or stopped.

Constructing a sender does not run it. A sender runs only after some caller
connects and starts it. The pipe operator is therefore a description of a
pipeline, not an immediate function call:

```cpp
auto sender = stdexec::just(40) |
    stdexec::then([](int value) noexcept { return value + 2; });
// No work has run yet.
```

The bridge's generated dispatch starts exported operations for you. Business
code normally writes a stdexec::task<T> coroutine and uses co_await for senders:

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
#include <chrono>
#include <string>

BRIDGE_ASYNC
stdexec::task<std::string> delayed_echo(std::string message) {
  co_await dcb::sleep(std::chrono::milliseconds(100));
  co_return message;
}
```

stdexec::task<T> is the v2 async return type for generated APIs. It is
scheduler-affine: once started by the bridge, the coroutine resumes on the
scheduler in its environment. Do not invent a bridge-specific Future type.

## Choosing a marker

| Marker | Runs on | Dart result | Use it for |
|--------|---------|-------------|------------|
| BRIDGE_SYNC | bridge io thread | T | Tiny, non-blocking work |
| BRIDGE_ASYNC | bridge io thread, as a coroutine | Future<T> | Timers, channels, async composition |
| BRIDGE_NORMAL | built-in blocking pool | Future<T> | Blocking I/O or CPU-heavy synchronous work |

BRIDGE_SYNC and BRIDGE_ASYNC must not call sleep_for, wait on a mutex,
perform blocking file I/O, or call dcb::sync_wait. Use BRIDGE_NORMAL or
dcb::spawn_blocking instead.

## Starting a sender explicitly

The built-in event loop is exposed as Runtime::io_scheduler(). The current
stdexec names used by this repository are starts_on, continues_on,
write_env, and read_env:

```cpp
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
#include <tuple>

auto sender = stdexec::just(40) |
    stdexec::then([](int value) noexcept { return value + 2; });

auto result = dcb::sync_wait(stdexec::starts_on(
    *dcb::Runtime::instance().io_scheduler(), std::move(sender)));

if (result) {
  int value = std::get<0>(*result);  // sync_wait returns optional<tuple<...>>
}
```

dcb::sync_wait blocks the calling thread and returns the stdexec result
shape (optional<tuple<...>>). It has an additional deadlock guard and throws
if called from the bridge io thread. Use it only from a worker or external
thread, never from BRIDGE_SYNC or an io-thread callback.

The scheduler combinators have different jobs:

- starts_on(sched, sender) chooses where the operation begins.
- continues_on(sender, sched) moves the downstream completion to sched.
- on(sched, sender) runs the operation on sched but returns to the starting
  scheduler for the continuation. It does not pin the entire rest of a
  pipeline to the target scheduler.
- stdexec::schedule(sched) creates a sender that represents one scheduled
  turn with no value.

For the bridge scheduler, generated dispatch normally already supplies
starts_on(io_scheduler, ...); add an explicit starts_on only when writing
your own launcher.

## Composing asynchronous operations

Inside a task, one-value senders are awaited as their bare value:

```cpp
BRIDGE_ASYNC
stdexec::task<int> total() {
  auto left = co_await read_left();
  auto right = co_await read_right();
  co_return left + right;
}
```

Use when_all when independent operations should run together. If each
sender produces one value, the values are flattened and can be structured
bound:

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> parallel_greeting() {
  auto [name, suffix] = co_await stdexec::when_all(
      load_name(), load_suffix());
  co_return name + suffix;
}
```

Useful composition tools:

| Tool | Meaning |
|------|---------|
| then | Map successful values |
| let_value | Return a new sender based on a previous value |
| when_all | Wait for all branches; failure requests stop on siblings |
| exec::when_any | First completion wins; losers receive a stop request |
| upon_error | Convert or log an error |
| upon_stopped | Provide a stopped fallback |

Exceptions thrown by a then callable become set_error and are eventually
encoded as a Dart error by generated dispatch.

## Blocking work and custom schedulers

dcb::spawn_blocking is the bridge helper for a synchronous callable that
belongs inside an async pipeline:

```cpp
#include <dart_cpp_bridge/runtime.hpp>

BRIDGE_ASYNC
stdexec::task<int> compute(int n) {
  auto value = co_await dcb::spawn_blocking([n] {
    return expensive_synchronous_work(n);
  });
  co_return value;
}
```

The callable runs on the runtime's blocking pool, while completion is moved
back to the bridge io scheduler. The default pool has four threads.

The pool size is configurable before the runtime starts:

```dart
await DcbLib.init(threadPoolSize: 8);
```

For a C++-owned runtime, call Runtime::set_pool_threads(8) before the first
start() or session open. The generated Dart DartCppBridge.init option is
poolThreads; it must be set before the first isolate starts the runtime.

spawn_blocking also accepts any stdexec scheduler. This is how to use a
separate pool for a specific workload:

```cpp
#include <exec/static_thread_pool.hpp>

namespace {
// Keep the pool alive until all operations using it have completed.
exec::static_thread_pool image_pool{8};
}

BRIDGE_ASYNC
stdexec::task<int> decode_image(std::uint64_t address, std::int32_t len) {
  auto pixels = co_await dcb::spawn_blocking(
      [address, len] { return decode_pixels(address, len); },
      image_pool.get_scheduler());
  co_return pixels;
}
```

BRIDGE_NORMAL always uses the built-in runtime pool. Choose BRIDGE_ASYNC plus
spawn_blocking(callable, custom_scheduler) when a particular operation needs
its own pool. The custom scheduler must outlive the sender and every operation
started from it.

## Timers

dcb::sleep uses the runtime's timed io scheduler and does not block a thread:

```cpp
BRIDGE_ASYNC
stdexec::task<void> wait_a_bit() {
  co_await dcb::sleep(std::chrono::milliseconds(50));
  co_return;
}
```

It can also accept a custom timed scheduler that implements
schedule_after(duration), as shown by the libuv example in
examples/foreign_runtime_demo.

## Cancellation and environments

Cancellation is cooperative. A stop request does not kill a C++ function; the
operation must observe the token and finish or propagate set_stopped.
Inject a token with the current write_env API:

```cpp
stdexec::inplace_stop_source source;

auto cancellable = stdexec::write_env(
    some_sender(),
    stdexec::prop{
        stdexec::get_stop_token,
        source.get_token()});

source.request_stop();
```

Channels, timers, and spawn_blocking participate in sender cancellation.
The Dart Future itself cannot be force-cancelled, so expose an application
operation ID and a cancel method when users need cancellation from Dart.
See [Channels](/dart_cpp_bridge/guides/fundamentals/channels/) for the
cancel-safe send/receive rules.

## Detached work and coroutine lifetime

Use a scope or a detached launcher for work that is intentionally not awaited.
The detached operation must own all state it needs:

```cpp
template <class S>
void launch_on_io(S sender) {
  exec::start_detached(
      stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
          std::move(sender)) |
      stdexec::upon_error([](std::exception_ptr error) noexcept {
        log_error(error);
      }));
}
```

Prefer structured concurrency (exec::async_scope or a counting scope) for
long-lived services and drain the scope before destroying its scheduler.
exec::start_detached and async_scope::spawn do not automatically turn an
unhandled error into a Dart response; attach upon_error or handle the error
inside the task.

Generated async dispatch uses a zero-capture coroutine IIFE. Do not copy that
pattern with a dangling capture:

```cpp
auto task = [](Request request) -> stdexec::task<void> {
  // All state is a parameter and therefore lives in the coroutine frame.
  co_await handle(std::move(request));
}(std::move(request));
```

## The repository's reference guide

This page is the usage path. For compile-verified details on completion
signatures, connect/start, async_scope, bulk, callback interop, and
common diagnostics, read the repository reference:
[docs/cpp26_executor_model_usage.md](https://github.com/deretame/dart_cpp_bridge/blob/main/docs/cpp26_executor_model_usage.md).

