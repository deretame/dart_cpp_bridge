---
title: Caveats & Gotchas
description: "A lookup page collecting common pitfalls: codegen constraints, threading rules, cancellation semantics, lifecycle, and C/foreign-runtime contracts"
---

This page collects the cross-cutting gotchas that bite new users. Each entry
links to the full explanation; keep the details on their home pages so this
page stays a lightweight index.

## Code Generation & API Headers

| Gotcha | What happens | Fix |
|---|---|---|
| Scanned API headers include third-party or dependency headers | libclang parses every transitive include; an unparseable header silently degrades template types (`std::vector`, `std::unordered_map`, ...) to `int` in generated bindings | Only include C++ standard headers, `dart_cpp_bridge/*`, and `async_simple/coro/Lazy.h`; put heavy includes in `native/api_impl/*.cpp`. See [Include Whitelist](../../codegen/configuration/#include-whitelist) |
| Implementation code in API headers | Generated bindings depend on implementation details and the headers become fragile | Declarations only; implementations go in `api_impl/*.cpp`. See [Declaration Hygiene](../../codegen/configuration/#declaration-hygiene) |
| Type aliases / `using namespace` in scanned headers | codegen cannot resolve aliases and may generate wrong types | Expand aliases and use fully qualified names |
| Editing `native/generated/` or `lib/src/native_gen/` | Files are overwritten on the next generation | Treat them as build artifacts. See [Project Structure](../fundamentals/project-structure/) |
| Expecting build hooks to re-run codegen | Native Assets hooks only compile and link; stale generated code silently ships | Run `dcb_gen_tool generate` after API header changes. See [Native Assets Build Hooks](../fundamentals/native-assets-hooks/) |

## Threading & Deadlocks

| Gotcha | What happens | Fix |
|---|---|---|
| Blocking the io thread | Freezes the whole event loop; every coroutine and callback stalls | Offload blocking work to `spawn_blocking` / the thread pool. See [Threading Rules](../fundamentals/runtime/#threading-rules) |
| `syncAwait` on the io thread | Self-deadlock: the awaited coroutine needs the io thread to resume | Call `syncAwait` only from non-io threads |
| `DartFn` inside `BRIDGE_SYNC` | Deadlock: Dart replies are delivered through the io thread | Use async markers (`BRIDGE_ASYNC` / `BRIDGE_NORMAL`) or `spawn_blocking`. See [Common Mistakes](../fundamentals/markers/#common-mistakes) |
| `RescheduleLazy::detach()` | Exceptions surface on the io thread and crash the process | Use `dcb::spawn_detached` |
| Coroutine lambdas on MSVC | Captured values are corrupted after suspension | Use static coroutine functions or pass values explicitly. See [Common Mistakes](../fundamentals/async-simple/#common-mistakes) |

## async-simple & uthread

| Gotcha | What happens | Fix |
|---|---|---|
| Using async-simple's `uthread` fibers in business code | uthread is not built or linked by dart_cpp_bridge (CMake `EXCLUDE_FROM_ALL`), is not supported on Windows, and its Darwin assembly selection ignores `CMAKE_OSX_ARCHITECTURES`, breaking macOS cross-architecture slices | Use only async-simple's header-only surface (`Lazy` / `Executor` / `Promise` / `Signal`); if you need fibers, use Boost.Fiber. See [Don't use uthread](../fundamentals/async-simple/#dont-use-uthread-use-boostfiber) |

## Cancellation & Streams

| Gotcha | What happens | Fix |
|---|---|---|
| Expecting Dart to force-cancel a Future | A Dart `Future` is only a listener; it cannot interrupt the running C++ coroutine | Expose cooperative cancellation: register an `async_simple::Signal` per task id and emit `Terminate` from a `cancelTask`-style API. See [Cancellation (Signal & Slot)](../fundamentals/async-simple/#cancellation-signal-slot) |
| `sleep()` that never wakes on cancel | The timer keeps running to completion if no signal is bound | Bind the coroutine chain with `Lazy::setLazyLocal(signal)` so `sleep()` is interruptible. See [Cancellable sleep](../fundamentals/async-simple/) |
| `collectAll` / `collectAny` without `Terminate` | The losing tasks keep running after the first result | Use `collectAll<Terminate>` / `collectAny<Terminate>` to cancel the losers. See [Cancelling the losers](../fundamentals/async-simple/) |
| Cancelling a Stream subscription | Only stops Dart-side delivery; C++ continues and late `add()` calls are silently dropped | Stop the producer explicitly, or accept fire-and-forget semantics. See [Streams](../fundamentals/markers/) |

## Lifecycle & Sessions

| Gotcha | What happens | Fix |
|---|---|---|
| Worker isolate calls `shutdown()` | Closes the main isolate's session and stops the runtime | Only the main isolate may shut down. See [Lifecycle Management](../fundamentals/lifecycle/) |
| Sharing Opaque objects across isolates | Handles are per-session; another isolate cannot use them | Keep objects inside their owning isolate |
| Using the bridge after `dispose()` | Calls fail; the session is gone | Re-init the session, or finish work before disposing |

## Pure C API (cbridge)

| Gotcha | What happens | Fix |
|---|---|---|
| Blocking inside a `dcb_invoke_dart_fn` callback | The callback runs on the bridge io thread; blocking freezes the loop | Do minimal work, then marshal. See [Behavior](../../guides/advanced/cbridge/) |
| Reusing an async op after completion / cancel | `op_id` is one-shot; later calls are no-ops | Create a new op per operation. See [Async Operation Primitives](../../guides/advanced/cbridge/) |
| Expecting pure C code to `co_await` | The C side only creates / completes / cancels ops; the awaiting side is a C++ coroutine | Bridge via `dcb::async_wait` inside a `Lazy` |

## Foreign Runtime

| Gotcha | What happens | Fix |
|---|---|---|
| Calling timer APIs off the loop thread | libuv / glib timer APIs are not thread-safe | `schedule_after` is invoked on the loop thread; keep it that way |
| `cancel_after` touching the timer directly | Races with the loop thread | Marshal cancellation to the loop; make it a safe no-op for fired or unknown handles. See [Timer Flow](../advanced/foreign-runtime/) |
| Unregistering with pending coroutines | Suspended coroutines are not resumed | Ensure channels are closed / tasks are finished before unregistering. See [Worker Contract](../advanced/foreign-runtime/) |

## Further Reading

- [Code Generation Configuration](../../codegen/configuration/)
- [async-simple Coroutines Primer](../fundamentals/async-simple/)
- [Built-in Runtime](../fundamentals/runtime/)
