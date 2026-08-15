---
title: Foreign Runtime Integration (v2)
description: How a libuv or other external event loop provides a stdexec scheduler
---

:::caution[v2 development line]
v2 no longer has `ForeignExecutor` or `foreign_runtime.h`. External event
loops integrate by exposing a normal stdexec scheduler. The old v1 registration
API is kept only in the v1 source archive.
:::

## The v2 model

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
