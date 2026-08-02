---
title: async-simple Coroutines Primer
description: "Basic usage of the async-simple coroutine library used by dart_cpp_bridge: Lazy, Executor, co_await, starting coroutines, and the threading model"
---

`dart_cpp_bridge`'s C++ async layer is built on [async-simple](https://alibaba.github.io/async_simple/). You don't need to be an async-simple expert to write business code, but this chapter covers the minimum knowledge you need to get started without reading the official docs.

## Core Concepts

async-simple is a C++20 coroutine library. The bridge mainly uses two concepts:

- **`async_simple::coro::Lazy<T>`** — A lazily started coroutine task. A function returning `Lazy<T>` does not execute immediately when called; it only starts when awaited with `co_await`, `.start()`, or `syncAwait()`.
- **`async_simple::Executor`** — The coroutine scheduler. `dcb::AsioExecutor` and `dcb::ForeignExecutor` are both implementations. After a coroutine suspends, the executor decides which thread resumes it.

## Don't use uthread (use Boost.Fiber)

async-simple also ships an optional stackful-fiber implementation named
**uthread**. `dart_cpp_bridge` deliberately does **not** build or link it:

- The runtime only uses async-simple's header-only surface (`Lazy`,
  `Executor`, `Promise`/`Future`, `Signal`); no bridge code references
  uthread.
- The uthread static/shared targets are excluded from the CMake build
  (`EXCLUDE_FROM_ALL`) in `dart/native/CMakeLists.txt`.
- uthread is not supported on Windows.
- Its Darwin assembly sources are selected by `CMAKE_SYSTEM_PROCESSOR` and
  ignore `CMAKE_OSX_ARCHITECTURES`, which breaks macOS cross-architecture
  slices.

If your business code needs fibers, use a dedicated fiber library such as
**Boost.Fiber** instead. Do not include `async_simple/uthread/*` headers and
do not link `async_simple` / `async_simple_static`.

## Writing a Coroutine

A business C++ function just needs to return `Lazy<T>`, use `co_await` to wait for other async operations, and use `co_return` to return results:

```cpp
#include "async_simple/coro/Lazy.h"

async_simple::coro::Lazy<std::string> greet(std::string name) {
  // co_await other Lazy, channel, sleep, etc.
  co_return "Hello, " + name;
}
```

:::caution

- Do not use a plain `return` inside a `Lazy<T>` function; you must write `co_return`.
- Do not treat a `Lazy` function as a synchronous call: `greet("world")` only returns a task that has not yet executed.

:::

## Starting a Coroutine

### Start with an executor bound (recommended)

```cpp
auto* ex = dcb::Runtime::instance().executor();

my_coroutine(args)
    .via(ex)                              // specify the scheduler
    .start([](async_simple::Try<T>&& t) {  // async callback
      if (t.hasError()) {
        // handle exception
      } else {
        // use t.value()
      }
    });
```

`.via(ex)` returns `RescheduleLazy<T>`. It does not execute immediately; instead, it posts the task to the executor and the executor thread will run it later.

### Synchronously wait from a non-coroutine context

```cpp
#include "async_simple/coro/SyncAwait.h"

auto result = async_simple::coro::syncAwait(
    dcb::spawn(my_coroutine()));
```

`dcb::spawn(...)` binds the `Lazy` to the Runtime's executor and returns `RescheduleLazy<T>`. `syncAwait` blocks the current thread until the result is available.

:::danger
`syncAwait` cannot be called on the io thread, because the awaited coroutine also needs the io thread to resume, causing a **self-deadlock**.
:::

### Fire and forget

```cpp
dcb::spawn_detached(my_coroutine());
```

Equivalent to `.via(ex).start([](auto&&){})`, but safer. Do not use async-simple's native `RescheduleLazy::detach()`, because it rethrows exceptions on the io thread and crashes the event loop.

## Does the Executor propagate along the co_await chain?

This is the most common question. The answer: **yes, but that does not mean every nested coroutine automatically runs on `ex`**.

```cpp
async_simple::coro::Lazy<std::string> outer() {
  auto a = co_await inner_a();            // inner_a inherits the current executor
  auto b = co_await co::oneshot::recv();  // channel resumes on the current executor too
  co_return a + b;
}

// bind ex at startup
outer().via(ex).start([](auto&&) {});
```

async-simple's `co_await` passes the current executor to the awaited object's `coAwait(Executor*)` method. Both `Lazy` and the bridge's `channel` implement this method, so they automatically use the same executor.

But if you write manually:

```cpp
inner_a().start([](auto&&){});  // no co_await, no executor inheritance
```

That is a different story.

## Common Awaitables

### Another Lazy

```cpp
async_simple::coro::Lazy<int> inner();

async_simple::coro::Lazy<int> outer() {
  auto v = co_await inner();  // inherits executor
  co_return v + 1;
}
```

### channel

```cpp
auto [tx, rx] = co::oneshot::channel<std::string>();
tx.send("hello");

auto value = co_await rx.recv();  // suspend, resume when received
```

### Promise / Future

If you need to **non-blockingly wait inside a coroutine for data returned by an external callback or another thread**, and want exceptions to propagate back into the coroutine automatically, use `async_simple::Promise<T>` + `async_simple::Future<T>` directly.

Basic pattern:

```cpp
async_simple::coro::Lazy<std::string> wait_for_callback() {
  async_simple::Promise<std::string> p;
  auto fut = p.getFuture();

  // hand the promise or callback to the external system
  register_callback([p = std::move(p)](std::string result) mutable {
    p.setValue(std::move(result));
  });

  // coroutine suspends until setValue / setException is called
  auto value = co_await std::move(fut);
  co_return value;
}
```

Key points:

- `co_await Future<T>` suspends the current coroutine and **does not occupy a thread**
- When the external system (callback, thread, event loop) finishes, it calls `p.setValue(...)` to resume the coroutine
- If the external system fails, call `p.setException(std::current_exception())`; the exception is rethrown at the `co_await`
- `Future<T>` supports only a single consumer and is single-use

### Returning results from a thread

This is the same mechanism used by `dcb::spawn_blocking` under the hood. You can also write it yourself:

```cpp
async_simple::coro::Lazy<int> fetch_from_thread() {
  async_simple::Promise<int> p;
  auto fut = p.getFuture();

  std::thread([p = std::move(p)]() mutable {
    try {
      int value = do_some_blocking_work();
      p.setValue(value);
    } catch (...) {
      p.setException(std::current_exception());
    }
  }).detach();

  co_return co_await std::move(fut);
}
```

### Bridging callback-based APIs

This is the core idea behind the pure C bridge (`cbridge_wait.hpp`):

```cpp
// Conceptual: maintain an op_id → Promise mapping in your own code
async_simple::coro::Lazy<std::vector<uint8_t>> wait_for_c_callback(
    uint64_t op_id) {
  auto fut = take_promise_for_op(op_id);  // retrieve the Future for this op from your own registry
  co_return co_await std::move(fut);
}
```

The external C code does not need to know about coroutines; it only needs to call `setValue` or `setException` when it has a result.

:::note
`take_promise_for_op` is not a bridge-provided API; it represents the registry logic you maintain yourself. See `cbridge_wait.hpp` for a concrete implementation of this pattern.
:::

### Void return values

If the result has no value, use `async_simple::Unit` instead of `void`. This is the same reason `spawn_blocking` does it: `Future<void>` does not call `Future::value()` after `co_await`, so an exception set via `setException` would be silently swallowed. `Future<Unit>` always goes through `value()`, so the exception is always rethrown.

```cpp
async_simple::coro::Lazy<> wait_for_event() {
  async_simple::Promise<async_simple::Unit> p;
  auto fut = p.getFuture();

  register_callback([p = std::move(p)]() mutable {
    if (ok) {
      p.setValue(async_simple::Unit{});
    } else {
      p.setException(std::make_exception_ptr(std::runtime_error("failed")));
    }
  });

  co_await std::move(fut);
  co_return;
}
```

### Relationship with spawn_blocking

`dcb::spawn_blocking` is essentially:

```cpp
async_simple::Promise<WireT> p;
auto fut = p.getFuture();
asio::post(rt.pool(), [f = std::forward<F>(f), p = std::move(p)]() mutable {
  try { p.setValue(f()); } catch (...) { p.setException(std::current_exception()); }
});
co_return co_await std::move(fut);
```

So if you just want to throw a callable onto the thread pool and `co_await` the result, use `spawn_blocking` directly. `Promise/Future` is for more flexible scenarios, such as threads you manage yourself, external event loops, or C callbacks.

### Async sleep

```cpp
#include "async_simple/coro/Sleep.h"

async_simple::coro::Lazy<> delayed() {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(100));
  co_return;
}
```

On `AsioExecutor`, `sleep` uses `asio::steady_timer` and does not occupy a thread.

## Cancellation (Signal & Slot)

A Dart `Future` produced by a bridge call maps to a running `Lazy` coroutine.
There is no Dart-side handle that can "kill" that coroutine, so **the bridge
does not support direct future cancellation**. The async-simple way is
cooperative: a task listens on an `async_simple::Signal` through its own
`async_simple::Slot`, and whoever wants to cancel emits
`SignalType::Terminate`.

### Cancel an async operation by id

Because a Dart `Future` has no identity the C++ side can look up later, keep a
global `task_id → Signal` registry yourself and cancel by id:

```cpp
std::mutex g_mu;
std::unordered_map<std::string, std::shared_ptr<async_simple::Signal>> g_signals;

// May be called from any thread (Signal::emits is thread-safe).
bool cancel_task(std::string id) {
  std::shared_ptr<async_simple::Signal> signal;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_signals.find(id);
    if (it == g_signals.end()) return false;
    signal = it->second;
  }
  return signal->emits(async_simple::SignalType::Terminate) !=
         async_simple::SignalType::None;
}
```

The coroutine registers its signal at startup and binds it to its own
coroutine chain with `Lazy::setLazyLocal`, so every nested
`async_simple::coro::sleep()` inherits the Slot and can be interrupted. The
exception is caught at the wire boundary, so Dart sees a `StateError`:

```cpp
async_simple::coro::Lazy<std::string> cancellable_task(std::string id) {
  auto signal = async_simple::Signal::create();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_signals[id] = signal;
  }
  // RAII guard: g_signals.erase(id) on exit (normal / cancelled / failed)
  co_return co_await cancellable_task_impl(id).setLazyLocal(signal.get());
}

async_simple::coro::Lazy<std::string> cancellable_task_impl(std::string id) {
  try {
    co_await async_simple::coro::sleep(std::chrono::seconds(30));
  } catch (const async_simple::SignalException&) {
    throw async_simple::SignalException(
        async_simple::SignalType::Terminate, "task cancelled: " + id);
  }
  co_return "done:" + id;
}
```

### The library supports cancellable sleep

`async_simple::coro::sleep()` asks the executor for
`schedule(Func, Duration, Slot*)`. `dcb::AsioExecutor` implements that
override: when the coroutine chain has a cancellation signal bound (via
`setLazyLocal`, or automatically inside `collectAll<Terminate>` /
`collectAny<Terminate>`), the executor registers a `SignalType::Terminate`
handler that cancels the underlying `asio::steady_timer`, and the sleep
awaiter throws `SignalException` immediately instead of waiting out the
duration. This is how `examples/codegen_demo` cancels `cancellable_task`; its
integration tests C01-C06 cover normal completion, cancel-by-id, prompt timer
interruption, per-id isolation, and unknown ids.

If you write your own awaiters (custom IO), you can still use the docs'
custom-awaiter pattern (`signalHelper{Terminate}.tryEmplace` /
`checkHasCanceled`); the executor fix covers `sleep()` / `after()`.

`ForeignExecutor` (non-asio event loops such as libuv) uses the loop's own
timer when the runtime registers the optional timer callbacks
(`dcb_foreign_register_ex`), and falls back to a waiter thread otherwise;
cancellable sleep works in both cases. See
[Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/).

See the official docs for the full model — `collectAny` / `collectAll`
structured cancellation, `Lazy::setLazyLocal`, `CurrentSlot`, `ForbidSignal`,
and custom awaiters:
[Signal and Cancellation](https://alibaba.github.io/async_simple/docs.en/SignalAndCancellation.html).

## Running Multiple Tasks (collectAll / collectAny)

When one bridge call needs to run several coroutines, use the `collectAll` /
`collectAny` family instead of hand-rolling counters and promises.

### collectAll — wait for every task

`collectAll` starts all tasks and waits until every one finishes. Results come
back as `Try<T>` values, so **a failing task does not throw out of
`co_await collectAll(...)`**; each error is captured in its own `Try`.

```cpp
// Variadic: Lazy<T1>, Lazy<T2>, ... → tuple<Try<T1>, Try<T2>, ...>
auto [a, b] = co_await async_simple::coro::collectAll(
    compute_int(), compute_string());

// Vector: std::vector<Lazy<T>> → std::vector<Try<T>>
std::vector<async_simple::coro::Lazy<int>> input;
input.push_back(compute_int(1));
input.push_back(compute_int(2));
auto results = co_await async_simple::coro::collectAll(std::move(input));
```

`collectAllPara` is the same API but submits the tasks to the current
executor, so their suspensions can overlap. Keep in mind that the bridge's
`AsioExecutor` is a single thread, so "para" means cooperative interleaving on
the io thread, not multi-core parallelism.

### collectAny — return the first completed task

`collectAny` starts all tasks and returns as soon as the first one completes;
the results of the others are ignored. The classic use is a timeout: race the
real work against `async_simple::coro::sleep(...)` and pick the winner.

```cpp
auto res = co_await async_simple::coro::collectAny(
    slow_http_fetch(), quick_cache_lookup());
if (res.index() == 0) { /* first argument won */ }
```

### Cancelling the losers: collectAll<Terminate> / collectAny<Terminate>

Both functions take a `SignalType` template parameter. When the first task
finishes, that signal is forwarded to the remaining tasks:

- `collectAny<SignalType::Terminate>` returns immediately, and the losers
  receive the cancellation signal. Sub-tasks that use
  `async_simple::coro::sleep(...)` (or any awaitable that checks the Slot)
  unwind automatically with `SignalException`; for custom IO, cooperate via
  `co_await CurrentSlot{}` and the `signalHelper` helpers (see the
  cancellation section above).
- `collectAll<SignalType::Terminate>` also signals the remaining tasks when
  the first one finishes, but **still waits for all of them to finish**; the
  cancelled tasks show up as `Try` errors in the result.

`examples/codegen_demo` exercises both APIs in `collect_all_demo`,
`collect_all_para_demo`, `collect_all_error_demo`, `collect_all_cancel_demo`,
`collect_any_demo`, and `collect_any_cancel_demo`; integration tests D01-D06
cover result collection, `Try` error capture, and loser cancellation.

See the official docs for `collectAllPara`, `collectAllWindowed`, and the
callback forms of `collectAny`:
[Lazy / Collect (EN)](https://alibaba.github.io/async_simple/docs.en/Lazy.html)
or [Lazy / Collect (中文)](https://alibaba.github.io/async_simple/docs.cn/Lazy.html).

## Blocking Operations

Never perform blocking IO or long-running computation directly in an io coroutine. Use `dcb::spawn_blocking`:

```cpp
async_simple::coro::Lazy<int> compute() {
  auto result = co_await dcb::spawn_blocking([] {
    // runs on the thread pool; blocking is OK
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 42;
  });
  co_return result;
}
```

## MSVC Coroutine Lambda Capture Bug

On MSVC, **do not capture variables in a coroutine lambda** (such as `std::string`, `DartFn`, or `shared_ptr`). After resumption, the captured values become garbage and cause `ACCESS_VIOLATION`.

```cpp
// ✗ crash
auto bad = [cb, input]() -> async_simple::coro::Lazy<> {
  co_await cb(input);
};

// ✓ correct
static async_simple::coro::Lazy<> good(
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  co_await cb(input);
}
```

This bug is unrelated to the executor; any coroutine lambda on any executor can trigger it. See [MSVC Notes](/dart_cpp_bridge/guides/advanced/foreign-runtime/#msvc-notes) for details.

## Common Mistakes

| Mistake | Cause |
| --- | --- |
| Calling `syncAwait` on the io thread | Deadlock; must be used on a non-io thread |
| Blocking the io thread in a coroutine | Freezes the entire event loop |
| Using `RescheduleLazy::detach()` | Exceptions are thrown on the io thread and crash the process; use `dcb::spawn_detached` |
| Capturing variables in a coroutine lambda (MSVC) | Captured values are corrupted after resumption, causing a crash |
| Calling a `Lazy<T>` function like a normal function | Only creates a task; it does not execute |

## Further Reading

- [async-simple official repository](https://github.com/alibaba/async_simple)
- [Signal and Cancellation (official docs)](https://alibaba.github.io/async_simple/docs.en/SignalAndCancellation.html) — Signal/Slot, structured cancellation, custom awaiters
- [Lazy / Collect (official docs)](https://alibaba.github.io/async_simple/docs.cn/Lazy.html) — collectAll / collectAny / collectAllPara / collectAllWindowed
- [Basic Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/) — Runtime, spawn, channel, sleep
- [Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/) — ForeignExecutor, MSVC notes
