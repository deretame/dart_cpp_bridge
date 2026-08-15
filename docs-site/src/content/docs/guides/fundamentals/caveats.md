---
title: Caveats & Gotchas (v2)
description: Common v2 codegen, threading, cancellation, lifecycle, and scheduler pitfalls
---

:::caution[v2 development line]
This page follows the stdexec implementation. For published v1 applications,
use [Versioned Documentation](/dart_cpp_bridge/versions/) and the v1 archive.
:::

## Code generation

| Gotcha | What happens | Fix |
|---|---|---|
| A scanned header includes an unparseable dependency | libclang may silently degrade template types in generated bindings | Keep scanned headers to standard headers, `dart_cpp_bridge/*`, and `stdexec/execution.hpp`; move heavy includes to `api_impl/*.cpp` |
| A type alias or `using namespace` is used | The parser may not resolve the public type | Use fully qualified concrete types |
| Generated files are edited by hand | The next generation overwrites the change | Edit API headers or implementations, then run `dcb_gen_tool generate` |
| A build hook is expected to regenerate code | Native Assets only compiles and links | Regenerate after API signature changes |

## Threading and deadlocks

| Gotcha | What happens | Fix |
|---|---|---|
| Blocking the io thread | The whole event loop stalls | Use `dcb::spawn_blocking` or `BRIDGE_NORMAL` |
| `dcb::sync_wait` on the io thread | Self-deadlock | Call it only from a worker or external thread |
| DartFn inside `BRIDGE_SYNC` | Dart cannot reply while the sync call blocks | Use `BRIDGE_ASYNC`, `BRIDGE_NORMAL`, or an explicit offload |
| A coroutine lambda captures request state | Lazy resumption can outlive the full expression | Use a zero-capture IIFE and pass state as parameters |

## Cancellation and streams

| Gotcha | What happens | Fix |
|---|---|---|
| Expecting Dart to force-cancel a Future | A Dart Future cannot interrupt native work | Expose a task ID and propagate a stdexec stop token |
| Cancelling a stream subscription | Dart stops receiving, but native work may continue | Stop the producer explicitly or accept late sink calls being dropped |
| Destroying a scheduler with pending work | Operations may retain scheduler references | Request stop and drain the structured-concurrency scope first |

## Lifecycle

| Gotcha | What happens | Fix |
|---|---|---|
| A worker isolate calls `shutdown()` | It closes every session and stops the process-wide Runtime | Only the main isolate calls `shutdown()` at process exit |
| An opaque object crosses isolates | Handles are scoped to their Session | Keep each object in its owning isolate |
| Native code is used after `dispose()` | The session is closed | Re-initialize or finish work before disposing |

## Pure C API and external runtimes

The C bridge API remains independent of the C++ async model. Its C side creates,
completes, and cancels operations; a C++ caller can await the result with
`dcb::async_wait`.

For libuv, glib, or custom loops, implement a plain stdexec scheduler. Do not
reintroduce the v1 `ForeignExecutor` registration API.

## Further reading

- [Code Generation Configuration](/dart_cpp_bridge/codegen/configuration/)
- [v2 stdexec Async C++](/dart_cpp_bridge/guides/fundamentals/stdexec/)
- [Built-in Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/)
- [Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/)
