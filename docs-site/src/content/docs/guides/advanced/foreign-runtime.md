---
title: Foreign Runtime Integration (v2)
description: How a libuv or other external event loop provides a stdexec scheduler
---

:::note[v2.1.0]
v2 no longer has `ForeignExecutor` or `foreign_runtime.h`. External event
loops integrate by exposing a normal stdexec scheduler. The old v1 registration
API is kept only in the v1 source archive.
:::

## Choose an integration path

There are two valid ways to connect an external asynchronous runtime to the
bridge. Choose the level that matches the API exposed by the external library:

1. **Recommended: adapt the runtime to a stdexec scheduler.** This is the best
   fit when you own the event loop or need to compose many operations on it.
2. **Lightweight: adapt callbacks into a sender pipeline.** If the library only
   exposes callback-based operations and you do not want to implement a
   scheduler, keep its runtime in control and use a channel sender as the
   boundary.

### Recommended: expose a scheduler

The bridge owns the Dart-facing Asio runtime. An application that already has a
libuv, glib, or custom loop should adapt that loop to the stdexec scheduler
concept and compose it with bridge senders.

```text
Your event-loop worker
  ├─ owns the loop thread
  ├─ queues work under a lock
  ├─ wakes the loop
  └─ drains queued functions on the loop thread
          │
          ▼
UvScheduler / custom stdexec::scheduler
          │
          ├─ schedule()       → run one sender step on the loop
          └─ schedule_after() → optional native timer support
          │
          ▼
stdexec::task and bridge channels
```

The working reference is
`examples/foreign_runtime_demo/native/uv_scheduler.hpp` together with
`uv_worker.hpp`. It is deliberately ordinary C++: there is no bridge-owned
foreign executor object and no C registration call.

### Lightweight: turn callbacks into a sender pipeline

You do not have to adapt every external runtime into a scheduler. For a
callback-only API, start the operation normally, send its completion into a
`co::oneshot` or `co::mpsc` channel, and compose the receiver with ordinary
stdexec operators. The external library keeps owning its event loop; the
channel is only the data boundary into the bridge pipeline.

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <dart_cpp_bridge/channel.hpp>
#include <stdexec/execution.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// An API supplied by the external library. The callback may run on any thread.
void external_fetch_async(
    std::string url,
    std::function<void(int, std::string)> callback);

BRIDGE_ASYNC
stdexec::task<std::string> fetch_with_callback(std::string url) {
  auto [tx, rx] = dcb::co::oneshot::channel<std::string>();
  auto callback_tx = std::make_shared<
      dcb::co::oneshot::Sender<std::string>>(std::move(tx));

  external_fetch_async(
      std::move(url),
      [callback_tx](int status, std::string body) {
        if (status == 0) {
          callback_tx->send(std::move(body));
        } else {
          callback_tx->send_error(std::make_exception_ptr(
              std::runtime_error("external request failed")));
        }
      });

  auto body = co_await (
      std::move(rx) |
      stdexec::then([](std::optional<std::string> value) {
        if (!value) {
          throw std::runtime_error("callback result was closed");
        }
        return std::move(*value);
      }) |
      stdexec::then([](std::string value) {
        // Apply any application-specific transformation here.
        return value;
      }));
  co_return body;
}
```

For a multi-event callback, use `co::mpsc` instead and choose the channel type
based on backpressure:

- `co::mpsc::unbounded`: `tx.send(value)` is an immediate, non-blocking call
  that returns `bool`, so a callback can call it directly. The trade-off is
  that the buffer can grow without a bound.
- `co::mpsc::bounded`: `tx.send(value)` returns a sender/awaiter because it
  may wait for capacity. It must be started or `co_await`ed in a sender/task
  context; do not call it from a callback and discard the returned sender. If
  the callback cannot await, hand the send operation to a task or scheduler,
  or use an unbounded channel at this boundary.

In both cases the task consumes `co_await rx.recv()` until the producer closes
the channel. If the awaiting operation is destroyed, a late oneshot send
returns `false`; the external library still needs its own cancellation or
lifetime policy if work must stop early.

This path is useful for C libraries, SDKs, and event loops whose public API is
already callback-shaped. It does not make the external runtime a stdexec
scheduler, so use the scheduler path when you need scheduling, timers, or
structured composition on that runtime itself.

## Business API

Include the stdexec execution header in scanned API headers and return
`stdexec::task<T>`:

```cpp
#pragma once

#include <dart_cpp_bridge/annotate.h>
#include <stdexec/execution.hpp>
#include <string>

BRIDGE_ASYNC
stdexec::task<std::string> ask_uv(std::string message);
```

The generated dispatch starts the task on the bridge scheduler. Inside the
business coroutine, use the external scheduler for work that belongs to the
foreign loop, then let the task resume on its home scheduler as required by the
task/sender composition.

## Scheduler responsibilities

A foreign scheduler must:

- be copyable and satisfy the stdexec scheduler requirements;
- make `schedule()` enqueue work without running user code inline;
- wake the event loop after enqueueing;
- invoke every queued continuation exactly once on the loop thread;
- keep queued operations alive until completion or cancellation;
- shut down only after pending operations and structured-concurrency scopes are
  drained.

Timer support is optional. If the scheduler implements `schedule_after`, bridge
timers can use the event loop's native timer; otherwise use an explicit timer
sender or a safe fallback in the application.

## Cross-runtime communication

Use `co::oneshot` and `co::mpsc` channels for values crossing worker
threads. Channels observe stop tokens, so cancellation can propagate without
blocking either loop.

When calling DartFn from a task, the bridge posts a Dart frame and awaits the
reply sender. Do not call a potentially suspending DartFn from
`BRIDGE_SYNC`.

## Shutdown order

Stop accepting new work, request cooperative cancellation, drain the async
scope, stop the foreign loop, and only then destroy the scheduler. Pending
stdexec operations may retain scheduler references.

## Migration from v1

| v1 | v2 |
| --- | --- |
| `ForeignExecutor` | user-provided `stdexec::scheduler` |
| `dcb_foreign_register*` | no bridge registration API |
| `foreign_runtime.h` | scheduler header in the application |
| `Signal / Slot` | `stop_token` |
| async-simple `Lazy` | `stdexec::task` |

For the complete sender and scheduler rules, read the
[stdexec async C++ guide](/dart_cpp_bridge/guides/fundamentals/stdexec/) and
[version guide](/dart_cpp_bridge/versions/).
