---
title: Built-in Runtime (v2)
description: The Asio and stdexec runtime used by the released v2.1.0 line
---

:::note[v2.1.0]
This page describes the current stdexec runtime. Published v1 applications use
the [v1 async-simple archive](/dart_cpp_bridge/v1/guides/fundamentals/async-simple/)
instead.
:::

`dart_cpp_bridge` includes a process-wide runtime built from Asio and stdexec.
`DartCppBridge.init()` starts it automatically. C++ business code can use the
runtime scheduler without creating another event loop.

## Runtime components

- **Asio `io_context`** — one runner by default for bridge dispatch and
  non-blocking async work; the runner count is configurable before startup.
- **`IoContextScheduler`** — the v2 stdexec scheduler returned by
  `Runtime::io_scheduler()`.
- **Asio thread pool** — the blocking scheduler returned by
  `Runtime::blocking_scheduler()`.
- **Channels** — `co::oneshot` and `co::mpsc` senders for cross-thread
  communication.

The runtime headers expose the selected Asio implementation through
`DCB_ASIO_NS`: it expands to `asio` by default and to `boost::asio` when
`DCB_USE_BOOST_ASIO=ON`. Use `DCB_ASIO_NS::...` in business code so the same
source works with either implementation. See [CMake dependency ownership and
Asio namespace](/dart_cpp_bridge/guides/fundamentals/native-assets-hooks/#cmake-dependency-ownership)
for host-provided stdexec/Asio configuration.

```cpp
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>

auto& runtime = dcb::Runtime::instance();
runtime.start();                         // normally done by Dart init
auto* io = runtime.io_scheduler();        // one runner by default
auto blocking = runtime.blocking_scheduler();
```

## Async business code

Use `stdexec::task<T>` for coroutine APIs exposed with `BRIDGE_ASYNC`:

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
#include <string>

BRIDGE_ASYNC
stdexec::task<std::string> delayed_echo(std::string message) {
  co_await dcb::sleep(std::chrono::milliseconds(100));
  co_return message;
}
```

The task is started on the bridge scheduler by generated dispatch. It suspends
without occupying the io thread. Use current stdexec names such as
`starts_on`, `continues_on`, `on`, `when_all`, and `sync_wait`; do not use the
v1 `Lazy` / `Executor` APIs.

## Blocking work

Never block the io thread. Use `dcb::spawn_blocking` from a task when the
operation must remain part of an async pipeline:

```cpp
BRIDGE_ASYNC
stdexec::task<int> compute(int n) {
  auto value = co_await dcb::spawn_blocking([n] {
    return expensive_synchronous_work(n);
  });
  co_return value;
}
```

For a wholly ordinary blocking function, use `BRIDGE_NORMAL`; generated
dispatch sends it to the runtime's blocking pool and still returns
`Future<T>` to Dart.

## Waiting from a non-coroutine function

`dcb::sync_wait(sender)` is a blocking convenience for non-coroutine callers.
It rejects calls made on any io scheduler runner because waiting there would
deadlock the event loop. A raw `stdexec::sync_wait` can finish with one spare
runner, but occupies every runner that calls it and deadlocks the scheduler if
all runners wait for work on that scheduler:

```cpp
auto result = dcb::sync_wait(
    stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                       stdexec::just(42)));
```

Use it from a worker or external thread, not from `BRIDGE_SYNC` business code.

## Channels and cancellation

```cpp
auto [tx, rx] = dcb::co::oneshot::channel<std::string>();
tx.send("hello");

// In a stdexec::task:
auto value = co_await std::move(rx);  // std::optional<std::string>
```

For mpsc channels, use `co_await rx.recv()`; the oneshot receiver is already a
sender. `recv()` is stop-token aware. v2 cancellation uses
`stdexec::inplace_stop_source` / `stdexec::stop_token`; cancelled operations
complete through `set_stopped()`. A Dart `Future` is not force-cancellable, so
applications that need cancellation should expose an explicit task ID and
cancel method.

## Scheduler configuration

The io scheduler uses one runner by default. Configure it before the first
session starts the runtime with `DartCppBridge.init(ioThreads: 2)`; C++ can
call `Runtime::set_io_threads(2)` before `start()`. The value is normalized to
one when zero is supplied and changes after startup are ignored.

## Pool configuration

The built-in blocking pool defaults to four threads. Configure it before the
first session starts the runtime with `DartCppBridge.init(poolThreads: 8)` (or
the generated `threadPoolSize` option). C++ can call
`Runtime::set_pool_threads(8)` before `start()`. For a separate pool per
workload, pass its scheduler as the second argument to `dcb::spawn_blocking`;
see [Threading and Blocking Work](/dart_cpp_bridge/guides/fundamentals/threading/).

## Threading rules

:::caution

- Never perform blocking I/O, sleep, or a blocking lock on an io scheduler runner.
- Do not call `dcb::sync_wait` from an io scheduler runner.
- Keep sender completion lambdas `noexcept` when passing them to detached or
  scope-owned operations.
- Drain structured-concurrency scopes before destroying their scheduler.
:::

## Examples and references

- `examples/base_demo` — runtime smoke tests and wire dispatch
- `examples/multi_runtime_demo` — independent runtime/channel patterns
- `examples/foreign_runtime_demo` — libuv exposed as a plain stdexec scheduler
- [v2 stdexec async C++](/dart_cpp_bridge/guides/fundamentals/stdexec/)
- [Channels](/dart_cpp_bridge/guides/fundamentals/channels/)
- [Threading and Blocking Work](/dart_cpp_bridge/guides/fundamentals/threading/)
- [Architecture](/dart_cpp_bridge/guides/fundamentals/architecture/)
- [Versioned Documentation](/dart_cpp_bridge/versions/)
