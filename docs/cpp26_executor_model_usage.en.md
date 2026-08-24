# C++26 Executor Model (`std::execution` / P2300) Usage Guide

> This document explains how to use the C++26 executor model (P2300
> `std::execution`, senders/receivers): starting work, using coroutine
> environments, launching from ordinary functions, fire-and-forget work,
> scheduler migration, cancellation, composition, structured concurrency,
> and wrapping callback-style C APIs as senders.
>
> All examples use the reference implementation cloned at
> `third_party/stdexec`. It covers P2300 as well as later task, scope, and
> environment proposals, and also contains non-standard `exec::` extensions.
> **Pinned baseline: commit `f0e8ae6f`** (approximately v0.11.0,
> nvhpc-26.05). stdexec's API changes quickly, so re-check examples after
> upgrading it.
>
> **A C++26 toolchain is not required.** stdexec requires C++20 (see
> [Section 1](#1-toolchain-and-integration)). The core patterns were checked
> against this checkout and the project's actual code under strict C++20.
> When moving to a standard-library implementation, **check the namespace,
> headers, constraints, and return types one by one**; do not mechanically
> replace `stdexec::` with `std::execution::`. `exec::` extensions generally
> have even less direct standard correspondence (see [1.4](#14-relationship-to-the-standard-library)).

## Contents

- [0. The one-sentence model](#0-the-one-sentence-model)
- [1. Toolchain and integration](#1-toolchain-and-integration)
- [2. Core concepts](#2-core-concepts)
- [3. Headers and namespaces](#3-headers-and-namespaces)
- [4. How to start: overview](#4-how-to-start-overview)
- [5. Starting from ordinary functions](#5-starting-from-ordinary-functions)
- [6. Starting in a coroutine](#6-starting-in-a-coroutine)
- [7. Moving between schedulers](#7-moving-between-schedulers)
- [8. Cancellation](#8-cancellation)
- [9. Composition](#9-composition)
- [10. Structured concurrency](#10-structured-concurrency)
- [11. Lifetime and RAII](#11-lifetime-and-raii)
- [12. Advanced facilities](#12-advanced-facilities)
- [13. Interoperation with callback APIs](#13-interoperation-with-callback-apis)
- [14. Common compiler errors](#14-common-compiler-errors)
- [15. Appendix: quick reference](#15-appendix-quick-reference)

---

## 0. The one-sentence model

A **sender** is a lazy recipe for an asynchronous computation: it describes
what to do but does not execute by itself. A **receiver** is the result sink
with three completion channels: `set_value`, `set_error`, and `set_stopped`.
A **scheduler** represents an execution context such as a thread pool or event
loop; `schedule(sched)` creates a sender that runs once on that context.
Connecting a sender to a receiver creates an **operation state**. Calling
`start` on that operation state starts the work.

```text
sender (recipe) --connect--> operation state --start--> execution --> receiver
```

The rules are:

- A sender is **lazy**: constructing it has no side effects. They begin only
  after `start`.
- A sender has value semantics and is **composable**. Adaptors such as `then`,
  `when_all`, and `upon_error` build larger senders.
- Completion is **unique**: an operation completes exactly once through value,
  error, or stopped. After the completion function is called, the operation
  state may immediately become invalid, so a completion callback must not use
  the receiver or operation state afterwards.
- Adaptors route child completion to the outer receiver, avoiding callback
  boilerplate in business code.

## 1. Toolchain and integration

### 1.1 C++20 is enough

stdexec backports P2300 to C++20; a C++26 toolchain is not required:

- The language standard is **C++20**. C++23 is needed only when the
  experimental `STDEXEC_BUILD_MODULES` option is enabled; it is off by default.
- Use a recent GCC, Clang, or MSVC. The exact matrix changes with the checkout;
  consult `third_party/stdexec/README.md` and its CI configuration. With this
  checkout on Windows, MSYS2 UCRT64 **GCC 16.1** can silently crash `cc1plus`
  while compiling stdexec. Clang 22 in the same environment compiles and runs
  the examples successfully. If GCC crashes, try Clang or MSVC.
- The CMake target supplies the important options automatically:
  - MSVC: `/Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /bigobj`
  - GCC: `-fcoroutines`
- It also propagates `Threads::Threads` (pthreads on Linux/macOS and no extra
  dependency on Windows).

### 1.2 CMake integration

```cmake
# Disable components that are unnecessary when stdexec is a subproject.
set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(STDEXEC_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/stdexec)

target_link_libraries(your_target PRIVATE STDEXEC::stdexec)
```

- stdexec is normally a header-only `INTERFACE` library;
  `STDEXEC::stdexec` is its alias target. This checkout requires CMake >= 3.28.
- Direct `-I third_party/stdexec/include` use is possible, but the CMake target
  is recommended because it supplies the compiler options above.
- Optional components include `STDEXEC_BUILD_PARALLEL_SCHEDULER`,
  `STDEXEC_ENABLE_ASIO`, `STDEXEC_BUILD_MODULES`, `STDEXEC_ENABLE_CUDA`, and
  `STDEXEC_ENABLE_IO_URING`; see `third_party/stdexec/CMakeLists.txt`.
- On Windows, `STDEXEC_ENABLE_WINDOWS_THREAD_POOL` is enabled when `windows.h`
  is detected.

### 1.3 Version pinning and API drift

The pinned checkout (`f0e8ae6f`) keeps deprecated aliases while standard names
change:

| Old name | Current name |
| --- | --- |
| `start_on` | `stdexec::starts_on` |
| `transfer` | `stdexec::continues_on` |
| `stdexec::read` | `stdexec::read_env` |
| `exec::write` / `exec::write_env` | `stdexec::write_env` |
| `exec::on` | `stdexec::on` |
| `exec::inline_scheduler` | `stdexec::inline_scheduler` |
| `stdexec::split` / `ensure_started` / `start_detached` | `exec::split` / `exec::ensure_started` / `exec::start_detached` |
| `exec/repeat_effect_until.hpp` | `exec/repeat_until.hpp` |
| `exec/system_context.hpp` / `get_system_scheduler` | `stdexec::get_parallel_scheduler` |

This guide uses the current names. `exec::reschedule` is an active extension,
not a deprecated alias.

### 1.4 Relationship to the standard library

- `stdexec::` is the implementation namespace for proposal facilities,
  primarily covering P2300, P3149, P3325, and P3552. It is **not character-
  for-character identical** to the current C++ working draft: for example,
  the draft places `sync_wait` in `std::this_thread`, while this checkout
  provides `stdexec::sync_wait`; headers, constraints, and implementation
  extensions may also differ. Check each API against the target standard
  library and current working draft.
- `exec::` is NVIDIA's experimental extension namespace
  (`experimental::execution`) and has no direct standard counterpart. It
  contains `async_scope`, `static_thread_pool`, `single_thread_context`, the
  old extension `task`, `when_any`, `split`, `ensure_started`, `start_detached`,
  `timed_thread_context`, `at_coroutine_exit`, and `create`. The idea of
  `async_scope` is standardized as `counting_scope`, but the APIs differ.
- `stdexec::task` is the scheduler-affine coroutine task from P3552, not part
  of P2300 itself. This project uses it consistently for asynchronous C++
  business code; `exec::task` appears only when discussing the old extension.

---

## 2. Core concepts

| Concept | Meaning | Concept / facility |
| --- | --- | --- |
| `sender` | Describes an asynchronous operation and its possible completion signatures. | `stdexec::sender` |
| `receiver` | Sink for `set_value(vs...)`, `set_error(e)`, and `set_stopped()`. | `stdexec::receiver` |
| `scheduler` | Execution context; `schedule(s)` returns a sender for one scheduled turn. | `stdexec::scheduler` |
| operation state | Result of `connect(s, r)`; started by `start(op)`; not movable. | `connect_result_t` |
| environment | Receiver context queried with `get_scheduler`, `get_start_scheduler`, `get_stop_token`, `get_allocator`, and so on. | `env_of_t` |
| completion signatures | Compile-time set such as `completion_signatures<set_value_t(int), set_error_t(std::exception_ptr)>`. | `completion_signatures_of_t` |
| sender adaptor | A composable operation such as `then`, `upon_error`, or `when_all`; supports `sndr | adaptor(f)`. | — |

Completion means:

- `set_value(vs...)`: success, optionally carrying values.
- `set_error(e)`: failure, usually carrying `std::exception_ptr`.
- `set_stopped()`: cancellation or intentional abandonment, with no value.
- A sender must make its resources ready before calling completion and discard
  its internal state immediately afterwards.

### 2.1 `sender` and `sender_in`: the environment is part of the type system

`stdexec::sender<S>` only says that `S` is a sender; it does not guarantee that
`S` can compute completion signatures or connect successfully in every receiver
environment. The concept that checks whether a sender is valid in a particular
environment is:

```cpp
template <class S>
  requires stdexec::sender_in<S, my_env_t>
void launch(S&& sndr);
```

- `sender_in<S, Env>` queries `completion_signatures_of_t<S, Env>` under
  `Env`. A sender that depends on a scheduler, stop token, or allocator can
  change its completion signatures—or become invalid—when the environment
  changes.
- Use `sender` for a generic recipe; when writing a launcher, scope wrapper,
  or custom receiver, prefer a `sender_in` constraint matching the real
  receiver environment so errors appear closer to the call site.
- Do not validate a sender only in an empty `env<>` and assume it works
  everywhere. `on()`, a task's home scheduler, and cancellable callbacks all
  depend on the actual environment.

---

## 3. Headers and namespaces

```cpp
#include <stdexec/execution.hpp>       // P2300 core algorithms (`stdexec::`)
#include <exec/static_thread_pool.hpp> // extensions (`exec::`)
```

| Namespace | Contents | Standard relationship |
| --- | --- | --- |
| `stdexec` (`STDEXEC`) | Proposal APIs: `schedule`, `then`, `when_all`, `sync_wait`, `just`, `on`, `starts_on`, `continues_on`, environment queries, `task`, `run_loop`, `spawn`, `spawn_future`, `counting_scope`, `read_env`, `write_env`, `prop`, `inline_scheduler`, and more. | Snapshot of P2300 + P3149 + P3325 + P3552 and related proposals |
| `exec` (`experimental::execution`) | Reference-implementation extensions: `static_thread_pool`, `single_thread_context`, `async_scope`, `start_detached`, `split`, `ensure_started`, `when_any`, `finally`, `repeat_until`, `unless_stop_requested`, `reschedule`, old extension `task`, timed facilities, `create`, and coroutine cleanup. | Non-standard extensions; some ideas have separate standard proposals |

Common headers:

```cpp
#include <stdexec/execution.hpp>           // core, sync_wait, task, run_loop,
                                           // spawn, scopes, env utilities
#include <exec/static_thread_pool.hpp>     // multi-thread pool
#include <exec/single_thread_context.hpp>  // single-thread context
#include <exec/start_detached.hpp>         // fire-and-forget
#include <exec/async_scope.hpp>            // structured concurrency
#include <exec/task.hpp>                   // old exec::task; not used by this project
#include <exec/reschedule.hpp>             // exec::reschedule
#include <exec/when_any.hpp>               // races
#include <exec/finally.hpp>                // cleanup sender
#include <exec/split.hpp>                  // broadcast sender
#include <exec/ensure_started.hpp>         // start and cache
#include <exec/repeat_until.hpp>           // repeat_until
#include <exec/unless_stop_requested.hpp> // stop check
#include <exec/just_from.hpp>              // lazy just
#include <exec/timed_thread_scheduler.hpp>// timed context
#include <exec/timed_scheduler.hpp>        // now/at/after CPOs
#include <exec/at_coroutine_exit.hpp>     // coroutine cleanup
#include <exec/on_coro_disposition.hpp>   // succeeded/stopped/failed cleanup
#include <exec/create.hpp>                // callback adapter
#include <exec/materialize.hpp>           // completion-as-value
#include <exec/trampoline_scheduler.hpp>  // recursive scheduling guard
#include <exec/asio/use_sender.hpp>       // Asio completion token
#include <exec/asio/asio_thread_pool.hpp> // Asio thread pool
#include <stdexec/stop_token.hpp>         // stop source/token/callback
```

> Do not include `stdexec/coroutine.hpp` directly; it contains internal
> facilities. `stdexec::task` comes from `<stdexec/execution.hpp>`. `exec::on`
> is deprecated; use `stdexec::on`. `exec::reschedule` is not deprecated.

---

## 4. How to start: overview

| Situation | Mechanism | Header | Lifetime |
| --- | --- | --- | --- |
| Synchronous wait | `stdexec::sync_wait(sndr)` | `stdexec/execution.hpp` | Stack; ends when the call returns. |
| Untracked fire-and-forget | `exec::start_detached(sndr)` | `exec/start_detached.hpp` | Heap; self-deleting. |
| Scope-owned fire-and-forget | `stdexec::spawn(sndr, token)` | `stdexec/execution.hpp` | Scope-owned. |
| Scope-owned result | `scope.spawn_future(sndr)` / `stdexec::spawn_future(sndr, token)` | Scope/execution headers. | Scope-owned. |
| Full manual control | `connect(sndr, rcvr)` + `start(op)` | `stdexec/execution.hpp` | Caller-owned. |

In a coroutine, a sender can be directly `co_await`ed; the coroutine itself is
then started by one of these mechanisms.

---

## 5. Starting from ordinary functions

### 5.1 `sync_wait`: block until a result

`sync_wait(sndr)` runs a sender and blocks the current thread:

```cpp
#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>

using namespace stdexec;

int main()
{
  exec::static_thread_pool pool{8};
  scheduler auto sch = pool.get_scheduler();

  sender auto work = schedule(sch)
    | then([] { return 13; })
    | then([](int x) { return x + 42; });

  auto [result] = sync_wait(std::move(work)).value();
  std::cout << result << '\n';
}
```

The return type is `std::optional<std::tuple<Ts...>>`. A stopped sender returns
`std::nullopt`; an error is re-thrown. Internally, `sync_wait` provides a
`stdexec::run_loop` environment answering `get_scheduler`,
`get_start_scheduler`, and `get_delegation_scheduler`, all pointing to that
loop. This lets `on()`, scheduler-affine tasks, and senders that explicitly
query the delegation scheduler return work to the waiting thread. **It does
not guarantee that every sender's final completion callback runs on the
waiting thread**: for example, `schedule(pool) | then(...)` may complete its
receiver directly on a pool thread. `sync_wait` merely drives the run loop
from the waiting thread and blocks until completion:

```cpp
// Zero-argument get_scheduler() is a sender, equivalent to read_env(get_scheduler).
auto [sch2] = sync_wait(get_scheduler()).value();
```

> Restrictions:
>
> - The sender must have exactly one `set_value` completion signature. A
>   flattened `set_value(x, y)` from `when_all(a, b)` counts as one. Several
>   successful value shapes require `sync_wait_with_variant()`.
> - It is not cancellable; the environment supplies `never_stop_token`.
> - `sync_wait` is a blocking API. It can technically be written at any
>   ordinary C++ call site, but **do not call it from a coroutine, a
>   single-threaded event loop, or its scheduler thread**. If the awaited work
>   needs that thread to make progress, the call self-deadlocks. In a
>   coroutine, use `co_await` directly.
>
> A multi-runner scheduler does not make this safe. `sync_wait` occupies the
> calling runner until it returns; it does not release that runner while it is
> waiting. One raw `stdexec::sync_wait` can complete when a spare runner is
> available to execute the awaited work, after which the blocked runner returns
> to the scheduler. If every runner waits for work queued on the same scheduler,
> all runners are occupied and the scheduler deadlocks. This repository's
> `dcb::sync_wait` therefore rejects calls from every io scheduler runner,
> regardless of the configured runner count.

### 5.2 `start_detached`: fire-and-forget

```cpp
#include <exec/start_detached.hpp>

exec::static_thread_pool pool{4};

void kick_off_logging(std::string msg)
{
  exec::start_detached(
    schedule(pool.get_scheduler())
    | then([msg = std::move(msg)]() noexcept {
        write_log(msg);
      }));
}
```

`start_detached` connects and starts immediately. Its operation state is heap
allocated and self-deletes after completion. It does not compile-time reject a
sender with `set_error`; in the pinned implementation the built-in receiver
calls `std::terminate()` on error. Use `stdexec::spawn` for compile-time
rejection, or add `upon_error` before detached work. Detached work has no scope
owner, so use `async_scope` or `spawn` when shutdown must wait for it.

> `then` and `upon_error` add `set_error_t(std::exception_ptr)` when their
> callback can throw. Mark final lambdas passed to `start_detached` or
> `stdexec::spawn` `noexcept` after earlier errors have been handled.

### 5.3 `stdexec::spawn` / `spawn_future`: standardized fire-and-forget

P3149's `stdexec::spawn` requires an asynchronous scope token:

```cpp
#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>
#include <exec/async_scope.hpp>

int main()
{
  exec::static_thread_pool pool{2};

  stdexec::counting_scope scope;
  stdexec::spawn(
    schedule(pool.get_scheduler()) | then([]() noexcept { /* ... */ }),
    scope.get_token());

  scope.close();
  sync_wait(scope.join());

  exec::async_scope scope2;
  scope2.spawn(schedule(pool.get_scheduler()) | then([] { /* ... */ }));
  sync_wait(scope2.on_empty());
}
```

`stdexec::spawn(sndr, token, env)` injects the extra environment and requires
at compile time that the sender cannot call `set_error`; otherwise its
diagnostic says `spawn expects a sender that cannot fail`. Internally it uses
`token.wrap(sndr)` and associates the child with the scope.
`stdexec::spawn_future(sndr, token)` returns an awaitable sender, similar to a
`std::async` future. `async_scope.spawn` is more convenient but does not reject
failures at compile time and terminates on error.

```cpp
exec::async_scope scope;
sender auto future = scope.spawn_future(
  schedule(pool.get_scheduler()) | then([] { return 42; }));
auto [n] = sync_wait(std::move(future) | stopped_as_optional()).value();
// future may be connected only once; nobody connecting it does not leak.
```

### 5.4 `connect + start`: complete manual control

```cpp
#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>
#include <latch>

using namespace stdexec;

struct my_receiver
{
  using receiver_concept = receiver_tag;
  std::latch& done;

  void set_value(int value) noexcept
  {
    std::cout << "got " << value << '\n';
    done.count_down();
  }

  void set_error(std::exception_ptr error) noexcept
  {
    try { std::rethrow_exception(error); }
    catch (std::exception const& ex) { std::cerr << "err: " << ex.what() << '\n'; }
    done.count_down();
  }

  void set_stopped() noexcept
  {
    std::cout << "stopped\n";
    done.count_down();
  }

  auto get_env() const noexcept
  {
    return prop{get_stop_token, never_stop_token{}};
  }
};

int main()
{
  exec::static_thread_pool pool{1};
  std::latch done{1};
  auto op = connect(schedule(pool.get_scheduler()) | then([] { return 7; }),
                    my_receiver{done});
  start(op);
  done.wait();
}
```

An operation state is not movable or copyable, must live until completion, and
must be started exactly once. `schedule(pool...)` queues asynchronously, so
`start()` can return before the callback runs; destroying `op` early is the
classic lifetime UB. This is the foundation for other start mechanisms, but
application code normally uses it only for custom receivers and callback
adapters.

### 5.5 `run_loop`: drive an event loop yourself

```cpp
#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>
using namespace stdexec;

int main()
{
  run_loop loop;
  exec::start_detached(
    schedule(loop.get_scheduler()) | then([&loop]() noexcept {
      std::cout << "tick\n";
      loop.finish();
    }));
  loop.run();
}
```

`run()` returns only after `finish()` has been called, the queue is empty, and
in-flight work is zero. `finish()` is safe from any thread and can be called
more than once. Calling it after `run()` deadlocks because `run()` cannot
return to execute the later call. This pattern is useful for a custom thread,
a GUI/game loop, or implementing a blocking wait; `single_thread_context` is
built around it.

---

## 6. Starting in a coroutine

### 6.1 This project uses `stdexec::task` consistently

`stdexec::task<T, TaskEnv = env<>>` is this checkout's implementation of the
P3552 scheduler-affine task. It is lazy and is itself a sender; this project's
business code, runtime, and code generator consistently return
`stdexec::task<T>`. Do not switch to the old extension `exec::task<T>` for
affinity: current `stdexec::task` automatically returns to its **home/start
scheduler** after each `co_await`.

```cpp
#include <stdexec/execution.hpp>
#include <exec/static_thread_pool.hpp>

using namespace stdexec;

auto fetch_and_add(exec::static_thread_pool& pool) -> stdexec::task<int>
{
  // The scheduled work completes on the pool; the task then returns to its home scheduler.
  int a = co_await (schedule(pool.get_scheduler())
                  | then([] { return 20; }));
  int b = co_await (schedule(pool.get_scheduler())
                  | then([] { return 22; }));
  co_return a + b;
}

int main()
{
  exec::static_thread_pool pool{2};
  auto [answer] = sync_wait(fetch_and_add(pool)).value();
  std::cout << answer << '\n';                       // 42
}
```

The task's home scheduler comes from `get_start_scheduler` in the receiver
environment that starts it; `sync_wait` supplies its own run-loop scheduler.
The task promise answers both `get_scheduler` and `get_start_scheduler` with
that scheduler, and propagates the allocator and stop token to the sender it
awaits.

### 6.2 The value, error, and stopped semantics of `co_await`

Inside `stdexec::task`, `co_await sndr` behaves as follows:

| Upstream completion | `co_await` behavior |
| --- | --- |
| `set_value()` | `void` |
| `set_value(v)` | **Bare `v`, not a tuple.** |
| `set_value(v1, v2, ...)` | `std::tuple<...>` (decayed). |
| `set_error(e)` | **Throws** `e`. |
| `set_stopped()` | Does not resume the current statement; stopped propagates outward. |

Unlike `sync_wait`, which always returns `optional<tuple<...>>`, `co_await` uses
a bare value for one value. Write `int a = co_await ...`; do not destructure a
single value as though it were a tuple.

`co_await` binds more tightly than `|`: `co_await a | then(f)` is parsed as
`(co_await a) | then(f)`. Always write `co_await (a | then(f))` for a pipeline.

When the awaited operation completes with `set_stopped()`, the task does not
execute the statements after the `co_await`. Stopped travels outward through
the promise's `unhandled_stopped()`, and the task sender ultimately calls
`set_stopped()` on its downstream receiver. The default completion signatures
of `stdexec::task<T>` are `set_value_t(T)` (or no argument for `void`),
`set_error_t(std::exception_ptr)`, and `set_stopped_t()`, so tasks can be used
directly with `when_all`, `then`, and `sync_wait`.

### 6.3 Querying the environment in a coroutine

Calling a query CPO with no arguments creates a `read_env(query)` sender that
can be awaited directly:

```cpp
auto query_env() -> stdexec::task<void>
{
  scheduler auto scheduler_value = co_await get_scheduler();
  auto token = co_await get_stop_token();
  (void)scheduler_value;
  (void)token;
  co_return;
}
```

- `get_scheduler()` is the task's current home scheduler;
  `get_start_scheduler()` returns the same scheduler here.
- `get_stop_token()` is linked to the parent environment. When the parent
  requests stop, the task and the sender it awaits share the cooperative
  cancellation chain.
- The zero-argument form also works in ordinary sender code, for example
  `sync_wait(get_scheduler())`.

### 6.4 Do work on another scheduler, then return home

`stdexec::task` has no need for `exec::reschedule_coroutine_on`. To run one
piece of work on a worker scheduler, await a sender started there; after the
await, the task's `affine` behavior restores the home scheduler:

```cpp
auto handle_request(exec::static_thread_pool& workers) -> stdexec::task<result>
{
  auto value = co_await stdexec::starts_on(
    workers.get_scheduler(),
    stdexec::just() | stdexec::then([] { return blocking_compute(); }));

  // We are back on the scheduler that started this task.
  co_return finish_on_home(std::move(value));
}
```

To change the **home scheduler for the entire task**, write
`starts_on(home, task())` at the outermost launch. Do not expect
`continues_on(target)` to permanently change the task's home; it only changes
where the awaited sender completes, after which the task resumes with its
affinity.

### 6.5 The hard `TaskEnv` and home-scheduler constraints

The second template parameter `TaskEnv` can customize the task allocator,
`start_scheduler_type`, `stop_source_type`, `error_types`, and extra
environment properties. The defaults are a byte allocator,
`stdexec::task_scheduler`, `inplace_stop_source`, `std::exception_ptr`, and
an empty extra environment. Most business code should keep the defaults.

The default `task_scheduler` type-erases the home scheduler and imposes two
implementation constraints on custom schedulers:

1. `schedule(home)` must be an **infallible** sender. Merely satisfying the
   `stdexec::scheduler` concept is not enough; if its completion signatures
   still include `set_error`, constructing the task home scheduler fails.
2. The type-erased schedule operation state must fit in a fixed inline buffer.
   The default `STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE` is 72 bytes. If a custom
   scheduler's opstate is larger, raise the macro consistently in every
   translation unit involved (this project sets it to 256; see
   12.6.2/12.6.6).

These are implementation constraints of this checkout's `task_scheduler`, not
general rules for deciding whether an ordinary scheduler is valid.

### 6.6 Starting a task: choose its home first, then handle all fallbacks

```cpp
// 1) Block from an ordinary thread; sync_wait's run_loop is the home.
auto [value] = sync_wait(my_task()).value();

// 2) Start on a specified event loop as home; handle non-value paths before detaching.
exec::start_detached(
  starts_on(io_scheduler, my_void_task())
  | upon_error([](std::exception_ptr error) noexcept { log_error(error); })
  | upon_stopped([]() noexcept { log_stopped(); }));

// 3) Track the lifetime in a scope.
exec::async_scope scope;
scope.spawn(my_void_task()
  | upon_error([](std::exception_ptr error) noexcept { log_error(error); }));

// 4) Track it in a scope and keep its result.
sender auto future = scope.spawn_future(my_task());
```

The generated wire dispatch in this project uses a fixed launcher: it first
sets the io scheduler as home with
`starts_on(Runtime::io_scheduler(), task)`, adds `noexcept`
`upon_error`/`upon_stopped` fallbacks, and finally calls `start_detached`. The
launcher checks the actual io environment with `sender_in<S, spawn_env_t>` at
compile time instead of checking only bare `sender<S>`.

### 6.7 Lazy coroutine lambdas must not use captures as coroutine state

This is a high-risk dangling-access trap that can surface in optimized builds:

```cpp
// Wrong: the lazy task outlives the temporary lambda; its captures dangle on first resume.
auto bad = [session, request]() -> stdexec::task<void> {
  co_await dispatch(session, request);
}();

// Correct: a captureless IIFE passes all state by value into the coroutine frame.
auto good = [](std::shared_ptr<Session> session, Request request)
              -> stdexec::task<void> {
  co_await dispatch(session, request);
}(std::move(session), std::move(request));
```

A coroutine lambda's captures belong to the closure object and are not
automatically copied into the coroutine frame; the closure may be destroyed
before the task starts. Coroutine **parameters** do enter the frame at call
time and live until completion. Prefer a named coroutine function, or a
captureless IIFE with explicit value parameters. Do not hide additional
captures inside the IIFE; they recreate the same risk.

---

## 7. Moving between schedulers

### 7.1 Migration algorithms

| Algorithm | Meaning | Pipeline form |
| --- | --- | --- |
| `stdexec::starts_on(sched, sndr)` | Start `sndr` on `sched`; the sender then schedules itself. | No pipeline form. |
| `stdexec::continues_on(sndr, sched)` | Move downstream work to `sched` after `sndr` completes. | `sndr | continues_on(sched)` |
| `stdexec::on(sched, sndr)` | Start on `sched`, then return to the start scheduler. | Function form. |
| `stdexec::on(sndr, sched, closure)` | Run `sndr`, apply `closure` on `sched`, then return home. | `sndr | on(sched, closure)` |
| `exec::reschedule` | Move to the receiver environment's `get_start_scheduler`. | `sndr | exec::reschedule()` |
| `stdexec::schedule_from` | Internal building block of `continues_on`. | Do not use directly. |

The important `on()` equivalence is:

```text
on(sched, sndr)          = continues_on(starts_on(sched, sndr), old_sched)
on(sndr, sched, closure) = sndr completes -> continues_on(..., sched) -> closure
                           -> continues_on(..., old_sched)
```

`old_sched` comes from `get_start_scheduler` in the downstream environment. A
bare root connection falls back to `inline_scheduler`; a custom environment
without one produces `_CANNOT_RESTORE_EXECUTION_CONTEXT_AFTER_ON_`.

```cpp
using namespace stdexec;

exec::static_thread_pool pool{4};
exec::single_thread_context ui;

sender auto pipeline =
  just()
  | on(pool.get_scheduler(),
       then([] { return heavy_compute(); }))
  | continues_on(ui.get_scheduler())
  | then([](int result) { update_ui(result); });

sync_wait(std::move(pipeline));
```

`starts_on` must be nested:

```cpp
sender auto sender_value =
  starts_on(pool.get_scheduler(),
            just(42) | then([](int x) { return x * 2; }));
```

`exec::reschedule()` is useful when a callback re-enters the sender world and
must return to the current thread pool or event loop. Each `on` or
`continues_on` generally costs a queue submission and wake-up, so avoid long
migration chains on hot paths.

### 7.2 `get_start_scheduler`: the home mechanism

`on()`, `exec::reschedule`, and `stdexec::task` all use
`get_start_scheduler(env)`, meaning “the scheduler where this operation was
started”:

- `sync_wait` answers `get_scheduler`, `get_start_scheduler`, and
  `get_delegation_scheduler` with its run-loop scheduler, so
  `sync_wait(on(pool, ...))` can return to the waiting thread.
- `stdexec::task` constructs its home scheduler from the parent receiver
  environment; its promise then answers both `get_scheduler` and
  `get_start_scheduler` with that home (see 6.4).
- `exec::reschedule` is `continues_on(sndr, special_scheduler)`, where the
  special scheduler reads `get_start_scheduler` from the receiver environment
  at connect time; without it, compilation fails with `_CANNOT_RESCHEDULE_`.
- In this extension, `get_scheduler` can fall back to
  `get_start_scheduler` when only the latter is present.

---

## 8. Cancellation

### 8.1 Cancellation infrastructure

```cpp
#include <stdexec/stop_token.hpp>

stdexec::inplace_stop_source source;
stdexec::inplace_stop_token token = source.get_token();
stdexec::inplace_stop_callback<F> callback{token, fn};
source.request_stop();
```

Every registered callback runs exactly once after `request_stop()`, possibly
synchronously on the requesting thread. Callback destruction is synchronized
with the source. A callback must be destroyed while its source is alive; do
not destroy the source from its callback.

### 8.2 Passing cancellation through the environment

```cpp
using namespace stdexec;

inplace_stop_source source;
sender auto work =
  schedule(pool.get_scheduler())
    | then([] { /* stoppable work */ })
    | exec::unless_stop_requested()
    | write_env(prop{get_stop_token, source.get_token()});
```

`get_stop_token(env)` falls back to `never_stop_token`. `write_env(sndr,
prop{...})` or its pipeline form injects an environment property; the current
name is `write_env`, not the deprecated `write` aliases.

### 8.3 Responding to cancellation in business code

```cpp
sender auto cancellable_loop(inplace_stop_token token)
{
  return schedule(pool.get_scheduler())
       | then([token] {
           for (int i = 0; i < 1'000'000; ++i)
           {
             if (token.stop_requested())
               return 0;
             work_unit(i);
           }
           return 1;
         });
}
```

Use `inplace_stop_callback` when a third-party operation needs an explicit
interrupt. See Section 13.3.

### 8.4 Turning stopped into a value or an error

```cpp
sender auto optional_result = stopped_as_optional(work);
sender auto error_result = stopped_as_error(work, my_error{"cancelled"});
sender auto callback_result =
  upon_stopped(work, [] { std::cout << "cancelled\n"; });
```

`stopped_as_optional` accepts only a sender with exactly one successful
completion signature, and that signature must carry exactly one value.
`set_value()`, multiple values, or multiple value shapes do not satisfy the
constraint; use `into_variant` first for a multi-shape result.

### 8.5 Timeouts and races with `when_any`

The early P2300 `stop_when(sndr, trigger)` was removed; stdexec has only an
internal `__stop_when`, not a public `stdexec::stop_when`. Use `when_any`:

```cpp
exec::timed_thread_context timer;

sender auto fetch_with_timeout()
{
  auto timeout = exec::schedule_after(timer.get_scheduler(), 200ms)
               | then([]() -> int { throw timeout_error{}; }); // C++20; throws -> set_error
  return exec::when_any(fetch_from_network(), std::move(timeout));
}
```

The first branch wins and the others receive a cooperative stop request. This
is **not a hard timeout**: the current implementation selects a winner when
the first result arrives but waits for all branches to finish before sending
that result downstream. A loser that ignores its token, blocks in a
non-cancellable I/O operation, or never calls back can keep `when_any` waiting;
`200ms` does not mean the caller must return in 200 ms.

A real deadline requires a cancellable underlying operation and a guarantee of
post-cancellation **silence/drain**—for example, close the socket or cancel
the Asio operation and wait for its completion handler. If the loser is
detached instead, shared state must decouple its resources from the call stack
and define how late callbacks are discarded; they must not keep referring to
local variables. Completion signatures are the union of branch signatures,
not a variant.

### 8.6 Cancellation in a coroutine

```cpp
auto guarded() -> stdexec::task<int>
{
  std::optional<int> result =
    co_await stopped_as_optional(expensive_work());
  co_return result.value_or(-1);
}
```

`stdexec::task` propagates the parent token; stopped travels through the
promise chain when an outer operation requests cancellation.

### 8.7 Cooperative channel cancellation (`co::mpsc`, verified)

`co::mpsc` park paths support stop tokens. The verified forms are:

```cpp
auto [tx, rx] = co::mpsc::bounded<int>(1);
stdexec::inplace_stop_source source;

auto result = stdexec::sync_wait(
  stdexec::write_env(tx.send(v),
                     stdexec::prop{stdexec::get_stop_token,
                                   source.get_token()}));

// A custom receiver may return
// stdexec::env{stdexec::prop{stdexec::get_stop_token, token}} from get_env().

auto operation = stdexec::connect(
  stdexec::starts_on(stdexec::inline_scheduler{},
    stdexec::write_env(task(),
      stdexec::prop{stdexec::get_stop_token, source.get_token()})),
  my_receiver{});
```

Before a value is claimed it belongs to the sender operation state. Stop or
destruction rolls it back, so it never enters the channel; a race is decided
under the channel lock. When `stdexec::task` receives stopped, its coroutine is
not resumed and statements after `co_await` do not execute. The stop source
must outlive registered operation states. `co::oneshot` intentionally does not
accept a stop token; DartFn replies use destruction as the fallback.

---

## 9. Composition

| Adaptor | Purpose |
| --- | --- |
| `just(vs...)` | Immediate synchronous completion with values. |
| `exec::just_from(f)` | Lazy `just`; call `f` only at start. |
| `then(sndr, f)` | Map successful values; throwing callbacks add an error channel. |
| `let_value(sndr, f)` | Dynamically return a new sender after success. |
| `upon_error(sndr, f)` | Recover from an error. |
| `upon_stopped(sndr, f)` | Provide a stopped fallback. |
| `when_all(sndrs...)` | Start all branches; complete only when all succeed; failure/stop requests stop on siblings and fails/stops the whole operation. |
| `exec::when_any(sndrs...)` | First completion wins and requests stop on the rest, but completion waits for all branches to drain. |
| `bulk(sndr, policy, shape, f)` | Parallel indexed work; domains choose chunks. |
| `starts_on` / `continues_on` / `on` | Choose start, downstream, or temporary scheduler. |
| `exec::split(sndr)` | Turn a single-consumer sender into a multi-subscriber sender; the first connect starts it and cached values are broadcast as `const T&`. |
| `exec::ensure_started(sndr)` | Start immediately and cache; the returned sender connects once and passes its successful value as `T&&` to that consumer. Use `split` for multiple subscribers. |
| `exec::finally(sndr, cleanup)` | Run a void cleanup sender on every path. |
| `exec::repeat_until(sndr, pred)` | Repeat until a predicate succeeds. |
| `exec::materialize` / `dematerialize` | Turn completion signals into values and back. |
| `into_variant(sndr)` | Merge multiple value signatures into a variant. |
| `stopped_as_optional` / `stopped_as_error` | Convert stopped to optional or error. |
| `write_env` / `read_env` | Inject or query environment properties. |
| `get_scheduler` / `get_stop_token` | Query the environment as senders. |

```cpp
sender auto download_and_parse(std::string url)
{
  return just(std::move(url))
       | let_value([](std::string value) {
           return download(value)
                | then([](std::vector<std::byte> raw) { return parse(raw); });
         });
}
```

`when_all` flattens successful values into one
`set_value(vs1..., vs2..., ...)`, not a tuple of tuples. With one value per
branch, `auto [a, b] = co_await when_all(x, y);` works directly.

`when_all` starts all child senders, but “concurrent start” does not imply
multi-core parallelism. If every branch uses the same single-threaded event
loop, they still make progress serially; actual parallelism is determined by
the schedulers used by the branches.

```cpp
using namespace stdexec;

sender auto parallel_double(exec::static_thread_pool& pool, span<int> data)
{
  return schedule(pool.get_scheduler())
       | bulk(stdexec::par, data.size(),
              [data](std::size_t i) { data[i] *= 2; })
       | then([data] { return sum(data); });
}
```

`bulk` calls `f(i, vs...)` per index. The pool domain chooses chunked or
unchunked execution; the pool also supports `bulk_chunked` with
`f(begin, end, vs...)`.

---

## 10. Structured concurrency

### 10.1 Why structured concurrency is needed

`start_detached` has no owner that can tell the caller when background work is
finished; process shutdown can therefore leave operations dangling. Manual
`connect + start` requires the caller to get every operation-state destruction
order right. Structured concurrency binds concurrent child tasks to a **scope
object** and provides an explicit drain/join protocol. It restores two
invariants:

1. A child cannot outlive its parent scope.
2. Failure and cancellation can propagate together through `request_stop()`.

> A scope destructor does **not** block to drain children. `exec::async_scope`
> only asserts that it is already empty; `counting_scope` calls
> `std::terminate()` when destroyed in an invalid state. The “no child outlives
> its parent” guarantee comes from calling `on_empty()` or `close() + join()`
> before destruction, not from the destructor doing the work for you.

### 10.2 `exec::async_scope` (the common choice)

```cpp
#include <exec/async_scope.hpp>

exec::static_thread_pool pool{4};

{
  exec::async_scope scope;
  scope.spawn(schedule(pool.get_scheduler()) | then(task_a));
  scope.spawn(schedule(pool.get_scheduler()) | then(task_b));

  sender auto future = scope.spawn_future(
    schedule(pool.get_scheduler()) | then(task_c));

  sync_wait(when_all(scope.on_empty(),
                     std::move(future) | then([](int value) { use(value); })));
}  // scope is empty here.
```

| Member | Purpose |
| --- | --- |
| `spawn(sndr, env = {})` | Fire and forget. It does not compile-time reject `set_error`; its receiver terminates on failure. Add `upon_error` or use `stdexec::spawn`. |
| `spawn_future(sndr, env = {})` | Start and return an awaitable sender; connect the result only once. Unconnected results are discarded without a leak. |
| `nest(sndr)` | Return a scope-adopted sender; registration occurs at connect and removal at completion. |
| `on_empty()` | Sender that completes with `set_value()` when the scope is empty; equivalent to `when_empty(just())`. |
| `when_empty(sndr)` | Wait for the scope to empty, then start `sndr`. |
| `request_stop()` | Request stop for all children through the scope token. |
| `get_stop_source()` / `get_stop_token()` | Access the scope stop state. |

A service can own a scope for its entire lifetime:

```cpp
struct server
{
  exec::async_scope scope;

  void start(exec::static_thread_pool& pool)
  {
    // accept_loop_sender must read the stop token and cancel the underlying
    // accept operation; a blocking while-loop inside then() will not stop
    // automatically when request_stop() is called.
    scope.spawn(accept_loop_sender(pool)
              | upon_error([](std::exception_ptr error) noexcept { log_error(error); }));
  }

  void shutdown()
  {
    scope.request_stop();
    sync_wait(scope.on_empty());
  }
};
```

The scope must be empty before destruction. Debug builds can assert this;
relying on destruction with active children is undefined behavior in release.

### 10.3 `stdexec::counting_scope` / `simple_counting_scope`

The C++26/P3149 scope facility is available as `stdexec::counting_scope`:

```cpp
using namespace stdexec;

counting_scope scope;
auto token = scope.get_token();
auto child = token.wrap(schedule(pool.get_scheduler()) | then(work));
exec::start_detached(std::move(child));    // Or sync_wait / connect+start.

scope.close();
sync_wait(scope.join());
scope.request_stop();                      // May be called before close.
```

| Member | Purpose |
| --- | --- |
| `get_token()` | Obtain the association token; `token.wrap(sndr)` binds stop propagation. |
| `token.try_associate()` | Manually associate; an exhausted count returns an empty association. |
| `max_associations()` | Defensive upper bound on associations. |
| `close()` | Stop accepting new associations. |
| `join()` | Sender that completes after all associated operations finish. |
| `request_stop()` | Cancel associated operations on `counting_scope`. |

`simple_counting_scope` has no stop source and its `wrap` does not forward
cancellation.

> **Destruction rule:** an untouched `unused` scope may be destroyed directly;
> `unused-and-closed` and `joined` are also valid terminal states. Once an
> operation has actually been associated, call `close()` and wait for `join()`
> (`sync_wait` or `co_await`) before destruction; every other state terminates
> with `std::terminate()`. Do not incorrectly say that an entirely unused
> scope must also be joined.

### 10.4 Nested scopes

The scope must be owned by the structure that waits for it. A coroutine frame
is a natural owner:

```cpp
auto phase(exec::static_thread_pool& pool) -> stdexec::task<void>
{
  exec::async_scope inner;
  inner.spawn(schedule(pool.get_scheduler()) | then(sub_task1));
  inner.spawn(schedule(pool.get_scheduler()) | then(sub_task2));
  co_await inner.on_empty();
  finalize_inner();
}

outer.spawn(phase(pool)
  | upon_error([](std::exception_ptr error) noexcept { log_error(error); }));
```

Do not put `inner` in an ordinary function local and pass only
`inner.on_empty()` to an outer scope. The function destroys `inner` while its
children may still be alive. `when_all(inner.on_empty())` holds a reference;
it does not extend the scope lifetime.

### 10.5 Relationship to `connect/start`

`async_scope::nest` is reference-counted `connect`: connect increments the
active count and completion decrements it. `spawn` adds a heap operation state
that self-deletes, while `spawn_future` adds shared result storage. Because
completion callbacks maintain the count, do not synchronously call something
that can deadlock from such a callback (for example,
`sync_wait(scope.on_empty())`).

---

## 11. Lifetime and RAII

1. **A sender is a value:** move it or capture it by value in a `then` lambda.
2. **An operation state is not movable:** the result of `connect` must remain
   in stable storage until completion.
3. **Lazy coroutine lambdas must not capture state:** a closure returning
   `stdexec::task` may be destroyed before its first resume. Do not use
   `[x] { co_await ...; }()` to store state. Use a named function, or a
   captureless IIFE that passes state by value into the coroutine frame (see
   6.7).
4. **Do not touch state after completion:** the operation state, receiver, and
   all operation-owned captures are dead after a completion method returns.
5. **The scope must outlive its children:** an active `async_scope` can assert
   or have release UB; counting scopes terminate if not joined.
6. **Stop callbacks have a source lifetime:** destroy callbacks before their
   source and never destroy the source from a callback.
7. **Handle errors:** `stdexec::spawn` rejects failures at compile time;
   `start_detached` and `async_scope::spawn` terminate on unhandled errors.
8. **Cancellation is cooperative:** `request_stop()` does not interrupt
   arbitrary C++ code; business code must inspect the token.

---

## 12. Advanced facilities

### 12.1 Timed scheduling: `timed_thread_context` + `schedule_after`

```cpp
#include <exec/timed_thread_scheduler.hpp>
#include <exec/timed_scheduler.hpp>

exec::timed_thread_context timer;
auto timed_scheduler = timer.get_scheduler();

sync_wait(exec::schedule_after(timed_scheduler, 200ms)
          | then([] { std::cout << "ding\n"; }));

sync_wait(exec::schedule_at(timed_scheduler,
                            exec::now(timed_scheduler) + 200ms)
          | then([] { /* ... */ }));
```

The context must outlive attached operations. Its scheduler provides
`now()`, `schedule()`, and `schedule_at()`; `schedule_after` is a CPO that can
fall back to `schedule_at(now + duration)`. Timed senders complete through
`set_value_t()` or `set_stopped_t()` and are cancellable. With an existing
Asio event loop, use `steady_timer + async_wait(use_sender)` instead.

### 12.2 `trampoline_scheduler`: prevent recursive stack overflow

`repeat_until` and polling chains can recursively schedule themselves on an
inline or single-thread scheduler. `trampoline_scheduler` queues schedules
beyond its depth/stack budget in a thread-local list and drains them from the
outermost frame:

```cpp
#include <exec/trampoline_scheduler.hpp>

exec::trampoline_scheduler trampoline;
sender auto sender_value = schedule(trampoline) | then([] { /* ... */ });
```

### 12.3 Coroutine cleanup: `at_coroutine_exit`

```cpp
#include <exec/at_coroutine_exit.hpp>
#include <exec/on_coro_disposition.hpp>

auto use_connection() -> exec::task<void>
{
  auto connection = co_await open_connection();

  co_await exec::at_coroutine_exit(
    [&connection]() -> exec::task<void> {
      co_await connection.async_close();
    });

  // Or select one disposition:
  // co_await exec::on_coroutine_succeeded(
  //   [&connection]() -> exec::task<void> { co_await connection.commit(); });

  co_await connection.async_use();
}
```

This is an extension paired with `exec::task`: `at_coroutine_exit` awaits
asynchronous cleanup for value, error, or stopped exit. Apple Clang is
unsupported; the header emits `#error` there.

It is not a general facility of P3552 `stdexec::task`. This project uses
`stdexec::task`, so do not assume the `exec::task` example can simply change
its return type. Prefer organizing asynchronous cleanup as an
`exec::finally(initial, cleanup)` sender chain, or explicitly await cleanup in
the business protocol. Ordinary synchronous resources still use normal RAII.

### 12.4 Process-wide parallel scheduler

```cpp
auto parallel_scheduler = stdexec::get_parallel_scheduler();
sender auto work = schedule(parallel_scheduler) | bulk(stdexec::par, n, f);
```

Enable `STDEXEC_BUILD_PARALLEL_SCHEDULER=ON` and link
`STDEXEC::parallel_scheduler`, or define
`STDEXEC_PARALLEL_SCHEDULER_HEADER_ONLY`. Windows uses `windows_thread_pool`
by default. The old `exec/system_context.hpp`,
`exec::get_parallel_scheduler`, and `get_system_scheduler` names are
deprecated.

### 12.5 Windows thread-pool backend

```cpp
#include <exec/windows/windows_thread_pool.hpp>

exec::__win32::windows_thread_pool pool;
// exec::__win32::windows_thread_pool pool{min_threads, max_threads};
auto scheduler = pool.get_scheduler();
```

The backend is enabled when `windows.h` is detected. `__win32` is an
implementation namespace and may change; portable code should use
`exec::static_thread_pool`.

### 12.6 Asio integration: `exec::asio`

The Asio adapter provides `use_sender` (a completion token) and
`asio_thread_pool` (an Asio-backed scheduler). Existing `asio::io_context`
applications normally need `use_sender` plus a thin scheduler wrapper.

#### 12.6.1 `use_sender`

```cpp
#include <exec/asio/use_sender.hpp>

asio::steady_timer timer{ioc, 200ms};
sender auto sender_value = timer.async_wait(exec::asio::use_sender);
// socket.async_read_some(buf, exec::asio::use_sender)
// produces a sender of (bytes_transferred).
```

| Asio completion | Sender completion |
| --- | --- |
| `ec == 0` | `set_value(args...)` |
| `operation_aborted` / `operation_canceled` | `set_stopped()` |
| Other non-zero `ec` | `set_error(std::exception_ptr)` containing `system_error` |

The adapter removes `error_code` from successful values and converts an outer
stop request to `emit(cancellation_type::all)` through Asio's
`cancellation_slot`.

#### 12.6.2 Wrap an existing `io_context` as a scheduler

```cpp
class io_context_scheduler
{
 public:
  using scheduler_concept = stdexec::scheduler_tag;

  explicit io_context_scheduler(asio::io_context& ioc) : ioc_(&ioc) {}

  stdexec::sender auto schedule() const noexcept
  {
    // post itself cannot fail and is not cancellable; use_sender still
    // conservatively declares error/stopped, so normalize both paths.
    return exec::asio::asio_impl::post(*ioc_, exec::asio::use_sender)
         | stdexec::upon_error([](std::exception_ptr) noexcept {})
         | stdexec::upon_stopped([]() noexcept {});
  }

  bool operator==(const io_context_scheduler&) const noexcept = default;

 private:
  asio::io_context* ioc_;
};
```

The underlying “run once on the event loop” sender is
`post(ioc, use_sender)`, but it cannot be used unchanged as a
`stdexec::task` home scheduler: the Asio adapter conservatively declares
error/stopped channels, while the default `task_scheduler` requires an
**infallible** `schedule()`. The wrapper above normalizes the theoretically
impossible error/stopped paths to `set_value()`.

`asio_impl` is a generated namespace alias: standalone Asio maps to `::asio`
and Boost.Asio to `::boost::asio`. After this adaptation,
`starts_on(sched, ...)`, `on(sched, ...)`, and `continues_on(sched)` can move a
pipeline to the io thread; `starts_on(sched, stdexec_task())` also makes it the
task's home, so the task returns to the io thread after awaiting other senders.

Merely satisfying `stdexec::scheduler<io_context_scheduler>` does not verify
the task constraints. Instantiate `starts_on(sched, task())` or check against
the corresponding `sender_in` environment. The adapted schedule opstate may
also exceed `task_scheduler`'s default 72-byte inline buffer; this project
defines `STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE=256`. If another compiler/Asio
combination still hits the opstate-size `static_assert`, increase it based on
the actual `sizeof` rather than moving a dangling object to the heap.

The scheduler captures an `io_context&`, so it must not outlive that context
(the project relies on Runtime member declaration order). Use
`asio::make_work_guard(ioc)` to keep a long-lived loop alive and reset the
guard during shutdown.

#### 12.6.3 Cancellable timer sleep

```cpp
sender auto sleep_on(asio::io_context& ioc,
                     std::chrono::milliseconds duration)
{
  auto timer = std::make_shared<asio::steady_timer>(ioc, duration);
  return timer->async_wait(exec::asio::use_sender)
       | then([timer] {});
}
```

This uses the application's event loop instead of a timer thread. A stop
request cancels the timer, producing `operation_aborted` and `set_stopped()`.
Keep the timer alive through completion; a local timer destroyed after
`async_wait` cancels the operation.

#### 12.6.4 `asio_thread_pool`

```cpp
#include <exec/asio/asio_thread_pool.hpp>

exec::asio::asio_thread_pool pool{4};
auto scheduler = pool.get_scheduler();
sync_wait(schedule(scheduler) | then([] { return 42; }));
```

The interface matches `exec::static_thread_pool`, with an
`asio::thread_pool` backend. `get_executor()` exposes its Asio executor, and
destruction stops and joins the pool.

#### 12.6.5 Default completion token

```cpp
auto timer2 = exec::asio::use_sender.as_default_on(
  asio::steady_timer{ioc, 100ms});
sender auto sender_value = timer2.async_wait();
```

The explicit form is
`exec::asio::as_default_on<exec::asio::use_sender_t>(io_object)`.

#### 12.6.6 CMake integration

`exec::asio` depends on generated `asio_config.hpp`, so configure it through
CMake rather than using only an include path:

```cmake
set(STDEXEC_ENABLE_ASIO ON CACHE BOOL "" FORCE)
set(STDEXEC_ASIO_IMPLEMENTATION "standalone" CACHE STRING "" FORCE)
# Use "boost" for Boost.Asio; standalone mode fetches asio-1.31.0.
target_link_libraries(your_target PRIVATE STDEXEC::asioexec)
# Compatibility alias: STDEXEC::asio_pool

# Only when a custom scheduler is used as a stdexec::task home and 72 bytes
# are actually insufficient. Propagate this to every downstream translation
# unit that instantiates stdexec::task or the scheduler.
target_compile_definitions(your_target PUBLIC STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE=256)
```

`STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE` changes the object layout of header
templates; inconsistent values across translation units create an ODR/ABI
risk. Put it on a shared CMake target with `PUBLIC` visibility rather than as a
private definition on one `.cpp`. This project propagates it through
`dcb_runtime`.

---

## 13. Interoperation with callback APIs

FFI, C callbacks, and platform APIs often use the pattern “register a callback,
then call back later”. Wrap that API as a sender and it can participate in all
the start, composition, cancellation, and scope facilities above. There are
two approaches: `exec::create` for a small wrapper, or a hand-written sender
for complete control.

### 13.1 `exec::create`: the short wrapper

```cpp
// Example C API:
// void dcb_fetch(int id, void (*cb)(void* user, int result), void* user);

#include <exec/create.hpp>

sender auto fetch_async(int id)
{
  return exec::create<set_value_t(int)>(
    [id]<class Context>(Context& context) noexcept {
      dcb_fetch(id, [](void* pointer, int result) noexcept {
        auto& current = *static_cast<Context*>(pointer);
        set_value(std::move(current.receiver), result);
      }, &context);
    });
}
```

The template arguments fix the completion signatures. If the C API can fail,
include `set_error_t(std::exception_ptr)` and call `set_error` in the callback.
The context address is safe to pass as `void*` because the operation state is
not movable, but the context becomes invalid immediately after completion.
The receiver must complete exactly once through value, error, or stopped.
The function passed to `create` runs synchronously during `start`, so it should
only register the callback, not perform expensive work; use `starts_on` or
`continues_on` to migrate execution.
When the start function returns `void`, `create` stores no additional state;
when it returns an object, `exec::create` stores that object in the operation
state until completion. Use this to hold registration handles and cancellation
callbacks with RAII (see 13.3).

The resulting sender behaves like any other sender:
`sync_wait(fetch_async(1))`, `co_await fetch_async(1)`, and
`scope.spawn(fetch_async(1) | upon_error(...))` are all valid.

### 13.2 A minimal hand-written sender

When the wrapper needs scheduler properties, complex completion signatures, or
connect-time work, implement the sender directly:

```cpp
struct tick_sender
{
  using sender_concept = stdexec::sender_tag;
  using completion_signatures =
    stdexec::completion_signatures<stdexec::set_value_t(int)>;

  template <class Receiver>
  struct opstate
  {
    using operation_state_concept = stdexec::operation_state_tag;
    Receiver receiver_;

    void start() & noexcept
    {
      // Start the asynchronous operation. On completion:
      stdexec::set_value(std::move(receiver_), 42);
    }
  };

  template <class Receiver>
  auto connect(Receiver receiver) const -> opstate<Receiver>
  {
    return {static_cast<Receiver&&>(receiver)};
  }
};
```

The `sender_concept`, `completion_signatures` (or a
`static consteval get_completion_signatures()`), and `connect` form the basic
sender contract. The operation state should be immovable in real code
(`STDEXEC_IMMOVABLE` is used inside the library), and `start() & noexcept` is
required. A sender may provide `get_env()` with
`get_completion_scheduler<set_value_t>` so `on()` and `continues_on` know its
completion context. Prefer `exec::create` unless these additional controls are
needed.

### 13.3 Making a wrapper cancellable

Read the stop token from the receiver environment in the `exec::create` start
function and forward stop requests to the underlying API. A key detail is that
the start function's return value is stored by `exec::create` in the operation
state until completion, so the cancellation handle and stop callback must be
owned by that returned state rather than local variables:

```cpp
sender auto fetch_cancellable(int id)
{
  return exec::create<set_value_t(int), set_stopped_t()>(
    [id]<class Context>(Context& context) noexcept {
      auto token = get_stop_token(get_env(context.receiver));

      // Assumed C API contract:
      // 1) the callback is not invoked inline before dcb_fetch returns;
      // 2) normal completion or cancellation eventually calls it exactly once;
      // 3) cancelled=true means cancellation is drained and user is no longer accessed.
      auto handle = dcb_fetch(
        id,
        [](void* user, int value, bool cancelled) noexcept {
          auto& current = *static_cast<Context*>(user);
          if (cancelled)
            set_stopped(std::move(current.receiver));
          else
            set_value(std::move(current.receiver), value);
          // After completion, current/the operation state may already be gone.
        },
        &context);

      struct cancel_fn
      {
        fetch_handle handle;
        void operator()() const noexcept { dcb_cancel(handle); }
      };
      using token_t = decltype(token);
      using callback_t = stdexec::stop_callback_for_t<token_t, cancel_fn>;

      struct state
      {
        fetch_handle handle;
        callback_t callback;
      };

      // create owns the return value; constructing the callback may invoke
      // cancel_fn synchronously when the token was already stopped.
      return state{handle, callback_t{token, cancel_fn{handle}}};
    });
}
```

The example is a contract sketch, not a universal template. Before adopting
it, verify:

- If `stop_possible()` is false, registration can be skipped. Generic code can
  use `stop_callback_for_t<Token, F>` and let the token type select the
  callback type. Also account for `stop_requested()` already being true:
  callback construction may invoke the callback synchronously.
- The safest model is for the stop callback to **only request cancellation**;
  the underlying API's single completion callback sends `set_stopped()` after
  the operation is truly quiet. Do not send `set_stopped()` immediately while
  still allowing a late C callback to reference `context`; the downstream may
  already have destroyed the operation state, causing UAF.
- A stop callback may win completion itself only if the cancellation API
  guarantees that, on return, callbacks are cancelled and none is running. It
  still needs atomic arbitration between stop and normal completion so the
  receiver is moved exactly once; no path may touch `context` afterwards.
- If registration may call back **synchronously**, the `exec::create` shortcut
  above is unsafe: the start function has not returned, its state is not yet in
  the operation state, and completion may destroy that state. Hand-write a
  sender/shared state, build the state first, then register the callback, and
  arbitrate synchronous completion separately.
- If the C API guarantees neither silence after cancellation nor a final
  callback, do not pass a raw `context` pointer to it. Use independent shared
  state with generation/atomic completion markers so late callbacks touch only
  that shared state, and define when the underlying handle is released. A
  hand-written sender is usually clearer than stacking more `exec::create`.

---

## 14. Common compiler errors

stdexec template diagnostics are organized as
`_WHAT_(...) / _WHY_(...) / _WHERE_(_IN_ALGORITHM_, ...) /
_WITH_ENVIRONMENT_(...)`. Recognizing that structure makes the error source
much easier to locate:

| Diagnostic (excerpt) | Source | Cause and fix |
| --- | --- | --- |
| `_CANNOT_RESTORE_EXECUTION_CONTEXT_AFTER_ON_` together with `_THE_CURRENT_EXECUTION_ENVIRONMENT_DOESNT_HAVE_A_SCHEDULER_` | `__on.hpp` | `on()` cannot find the home scheduler. Use `sync_wait` or provide `get_start_scheduler` in the receiver environment. |
| `_CANNOT_RESCHEDULE_` | `exec/reschedule.hpp` | `exec::reschedule` needs the environment's `get_start_scheduler`; solve it as above. |
| `stdexec::sync_wait() ... cannot complete successfully ... exactly one ... set_value_t(...)` | `__sync_wait.hpp` | The sender has no successful value path, usually only stopped/error. |
| `...can complete successfully in more than one way. Use stdexec::sync_wait_with_variant()` | `__sync_wait.hpp` | Several value shapes; use `sync_wait_with_variant()`. |
| `spawn expects a sender that cannot fail` | `__spawn.hpp` | The sender has a `set_error` signature. Handle it with `upon_error` and make the final callback `noexcept`. |
| `_INVALID_ARGUMENT_TO_THE_FINALLY_ALGORITHM_` / `_THE_FINAL_SENDER_MUST_BE_A_SENDER_OF_VOID_` | `__finally.hpp` | The cleanup sender is not a void sender; cleanup must only have `set_value_t()`. |
| Cannot construct `task_scheduler` from a custom scheduler / constructor constraints not satisfied | `__task_scheduler.hpp` | `schedule(sched)` still declares an error channel and violates the infallible home-scheduler constraint; normalize impossible error/stopped paths or use another scheduler. |
| `operation state ... too large to fit in the preallocated storage of task_scheduler` | `__task_scheduler.hpp` | The home scheduler's opstate exceeds the default 72 bytes; consistently raise `STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE` in all related translation units (see 12.6.6). |
| `sender_in<S, Env>` / completion-signature query fails | Sender constraints | The sender's scheduler, stop token, or allocator is absent from the actual environment; check the real receiver environment rather than only `sender<S>`. |

---

## 15. Appendix: quick reference

### Starting

```cpp
sync_wait(sndr);                              // Blocking wait; variant for multiple shapes.
exec::start_detached(sndr);                   // Untracked; unhandled errors terminate.
stdexec::spawn(sndr, scope.get_token());      // Scope-owned; rejects failing senders.
scope.spawn(sndr);                            // async_scope-owned; handle errors first.
scope.spawn_future(sndr);                     // Scope-owned result; connect once.
connect(sndr, rcvr) -> start(op);             // Manual; op must live through completion.
run_loop loop; loop.get_scheduler();          // Manually driven event loop.
co_await sndr;                                // Bare value for one value, tuple for many.
stdexec::task<T> task();                      // This project's only task type; task is a sender.
starts_on(home, task());                      // Choose the task's home scheduler.
```

### Schedulers

```cpp
exec::static_thread_pool pool{N};
auto scheduler = pool.get_scheduler();
auto scheduler0 = pool.get_scheduler_on_thread(0);
exec::single_thread_context single;
stdexec::run_loop loop;
stdexec::inline_scheduler{};
exec::timed_thread_context timer;
exec::trampoline_scheduler trampoline;
stdexec::get_parallel_scheduler();
exec::asio::asio_thread_pool asio_pool{N};
// Existing asio::io_context: schedule() returns
//   post(ioc, use_sender) | upon_error(noexcept) | upon_stopped(noexcept)
// For a large task-home opstate, define STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE
// consistently (see 12.6.2/12.6.6).
```

### Migration

```cpp
stdexec::on(scheduler, sndr);                    // Start there, then return home.
sndr | on(scheduler, closure);                  // Apply closure there, then return.
stdexec::starts_on(scheduler, sndr);             // Start there.
sndr | continues_on(scheduler);                 // Move downstream work there.
sndr | exec::reschedule();                      // Return to env start scheduler.
co_await starts_on(worker, work);              // stdexec::task visits worker, then returns home.
```

### Cancellation

```cpp
stdexec::inplace_stop_source source;
source.get_token();
source.request_stop();
sndr | write_env(prop{get_stop_token, token});   // The name is write_env.
get_stop_token(env);
read_env(get_stop_token);                        // Sender form; read is old.
exec::unless_stop_requested(sndr);
stopped_as_optional(sndr);
stopped_as_error(sndr, error);
upon_stopped(sndr, handler);
exec::when_any(a, b);                             // Cooperative race; request stop and drain losers.
scope.request_stop();
```

### Timers

```cpp
exec::timed_thread_context timer;
auto timed_scheduler = timer.get_scheduler();
exec::schedule_after(timed_scheduler, 200ms);
exec::schedule_at(timed_scheduler,
                  exec::now(timed_scheduler) + 200ms);
```

### Structured concurrency

```cpp
exec::async_scope scope;
scope.spawn(s);
scope.spawn_future(s);
scope.nest(s);
sync_wait(scope.on_empty());
scope.when_empty(sndr);
scope.request_stop();
// Drain before destruction.

stdexec::counting_scope counting;
auto token = counting.get_token();
auto wrapped = token.wrap(s);
counting.close();
sync_wait(counting.join());
counting.request_stop();
// Once an operation has been associated, close()+join() must finish before destruction.
```

---

## References

- This repository's `third_party/stdexec` at `f0e8ae6f` (approximately v0.11.0,
  nvhpc-26.05); examples are under `third_party/stdexec/examples/`, including
  `hello_world.cpp`, `hello_coro.cpp`, and `scope.cpp`.
- **P2300** `std::execution` senders/receivers, merged into the C++26 working
  draft in 2024. It supplies `schedule`, `then`, `when_all`, `on`, `starts_on`,
  `continues_on`, and `sync_wait` among other core facilities.
- **P3149** `async_scope`, the source of `spawn`, `spawn_future`,
  `counting_scope`, and `simple_counting_scope`; it was progressing toward
  C++26 in 2025.
- **P3325**, “A Utility for Creating Execution Environments”, the source of
  `prop`, `env`, and `write_env` environment utilities.
- **P3552R3**, [“Add a Coroutine Task Type”](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3552r3.html),
  the source direction for scheduler-affine `task` / `affine`; this checkout's
  `stdexec::task` follows that direction.
- [The current C++ working-draft execution/task section](https://eel.is/c++draft/exec.task),
  useful for checking standard namespace and semantics. The vendored checkout
  remains authoritative for the interfaces used by these examples.
- `third_party/stdexec/README.md`, for compiler support and integration via
  CPM, `add_subdirectory`, Conan, or a manual include path.
