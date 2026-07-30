---
title: async-simple Coroutines Primer
description: "Basic usage of the async-simple coroutine library used by dart_cpp_bridge: Lazy, Executor, co_await, starting coroutines, and the threading model"
---

`dart_cpp_bridge`'s C++ async layer is built on [async-simple](https://alibaba.github.io/async_simple/). You don't need to be an async-simple expert to write business code, but this chapter covers the minimum knowledge you need to get started without reading the official docs.

## Core Concepts

async-simple is a C++20 coroutine library. The bridge mainly uses two concepts:

- **`async_simple::coro::Lazy<T>`** — A lazily started coroutine task. A function returning `Lazy<T>` does not execute immediately when called; it only starts when awaited with `co_await`, `.start()`, or `syncAwait()`.
- **`async_simple::Executor`** — The coroutine scheduler. `dcb::AsioExecutor` and `dcb::ForeignExecutor` are both implementations. After a coroutine suspends, the executor decides which thread resumes it.

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

### Async sleep

```cpp
#include "async_simple/coro/Sleep.h"

async_simple::coro::Lazy<> delayed() {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(100));
  co_return;
}
```

On `AsioExecutor`, `sleep` uses `asio::steady_timer` and does not occupy a thread.

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
- [Basic Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/) — Runtime, spawn, channel, sleep
- [Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/) — ForeignExecutor, MSVC notes
