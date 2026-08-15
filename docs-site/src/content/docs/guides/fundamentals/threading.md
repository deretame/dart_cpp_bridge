---
title: Threading and Blocking Work
description: Runtime threads, spawn_blocking, pool sizing, and custom schedulers
---

The v2 runtime has two built-in execution contexts:

| Context | Threads | Purpose |
|---------|---------|---------|
| io scheduler | one Asio event-loop thread | Bridge dispatch, timers, non-blocking coroutine continuations |
| blocking scheduler | configurable Asio thread pool | BRIDGE_NORMAL and the default spawn_blocking work |

The io thread is intentionally single-threaded. This makes bridge dispatch and
session state predictable, but it means one blocking call can stop every
isolate's async progress.

## Select the right entry point

Use BRIDGE_SYNC only for tiny operations that cannot block. Use BRIDGE_NORMAL
for a complete synchronous operation:

~~~cpp
BRIDGE_NORMAL
std::string read_file(std::string path) {
  return read_entire_file(path);  // allowed to block
}
~~~

The generated Dart API returns Future<String>, while the native function runs
on the built-in blocking scheduler.

Use BRIDGE_ASYNC plus spawn_blocking when the blocking part is only one stage
of a larger async workflow:

~~~cpp
BRIDGE_ASYNC
stdexec::task<Decoded> load_and_decode(std::string path) {
  auto bytes = co_await dcb::spawn_blocking(
      [path = std::move(path)] {
        return read_entire_file(path);
      });
  co_return co_await decode_async(std::move(bytes));
}
~~~

The callable runs on the blocking pool. When it completes, the sender moves
back to the bridge io scheduler, so the task can continue without blocking it.

## Configure the built-in pool

The default pool size is four threads. Configure it before the runtime starts:

~~~dart
await DcbLib.init(threadPoolSize: 8);
~~~

For the lower-level DartCppBridge API, the option is poolThreads:

~~~dart
await DartCppBridge.init(
  bindings: createBindings(),
  poolThreads: 8,
);
~~~

For a C++-managed runtime, call Runtime::set_pool_threads before Runtime::start
or before the first session open. A value of zero is normalized to one thread.
Changing the value after startup has no effect on the already-created pool.

This changes the concurrency of the built-in pool globally. It does not create a
new pool per function.

## Use a custom pool for one workload

spawn_blocking accepts a scheduler argument. A static thread pool is a useful
choice for a workload that needs isolation from general bridge work:

~~~cpp
#include <exec/static_thread_pool.hpp>

namespace {
exec::static_thread_pool codec_pool{8};
}

BRIDGE_ASYNC
stdexec::task<int> decode_on_codec_pool(
    std::uint64_t address, std::int32_t length) {
  co_return co_await dcb::spawn_blocking(
      [address, length] {
        return decode_native_buffer(address, length);
      },
      codec_pool.get_scheduler());
}
~~~

The custom scheduler is local to this operation; BRIDGE_NORMAL still uses the
built-in pool. Keep codec_pool alive until every sender and operation using its
scheduler has completed. For a service, store the pool in the service object
and stop/join it during service shutdown.

The scheduler can be any type satisfying the stdexec scheduler contract. This
includes an Asio adapter, a static thread pool, or a foreign event-loop
scheduler such as the one in the
[foreign runtime example](/dart_cpp_bridge/guides/advanced/foreign-runtime/).

## sync_wait restrictions

dcb::sync_wait is for a non-coroutine caller on a non-io thread:

~~~cpp
auto result = dcb::sync_wait(
    stdexec::starts_on(
        *dcb::Runtime::instance().io_scheduler(),
        stdexec::just(42)));
~~~

It blocks the caller until completion and returns an optional tuple. Calling it
from the io thread throws instead of self-deadlocking. In a BRIDGE_ASYNC task,
use co_await; in BRIDGE_NORMAL, sync_wait is allowed if the awaited sender does
not create an application-level cycle.

## Thread-safety checklist

- Treat all bridge io callbacks as running on one thread.
- Do not call sleep_for, wait on a mutex, perform blocking I/O, or use
  sync_wait on that thread.
- A channel sender can send from a pool thread; an mpsc receiver remains
  single-consumer.
- Keep pointer buffers alive for the whole native operation; this is especially
  important when the operation is moved to a pool.
- Completion may return to io_scheduler after spawn_blocking. Protect shared
  application state if another thread also accesses it.
- Drain async scopes and join custom pools before destroying their schedulers.

## Related APIs

- [stdexec in v2](/dart_cpp_bridge/guides/fundamentals/stdexec/)
- [Built-in Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/)
- [Channels](/dart_cpp_bridge/guides/fundamentals/channels/)
- [Large-buffer pointer mapping](/dart_cpp_bridge/codegen/type-mapping/#large-buffers-address-passing-mode)

