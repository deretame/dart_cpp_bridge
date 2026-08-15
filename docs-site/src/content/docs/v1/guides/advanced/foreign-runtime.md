---
title: Foreign Runtime Integration
description: How non-Asio event loops (libuv, glib, etc.) connect to the main
  scheduler and call Dart callbacks
slug: v1/guides/advanced/foreign-runtime
---

This page explains how to let **non-Asio event loops** (libuv, glib, custom loops, etc.) integrate with the bridge coroutine system and call Dart-registered callback functions (DartFn) from an external runtime.

## Background

The bridge's core scheduling is based on `asio::io_context` + `AsioExecutor`. In real projects, however, the C++ side may already have its own event loop (e.g. libuv). It needs to:

1. Communicate with the bridge's channel/coroutine system
2. Call callbacks registered on the Dart side

The integration method: write a **Worker class** that wraps your event loop, implement a fixed-pattern schedule callback (lock and enqueue → wake loop → drain execution), and register it with the bridge. The bridge will create a `ForeignExecutor` (an `async_simple::Executor` implementation) internally, after which channels and coroutines can be scheduled transparently onto your loop thread.

## Architecture

```text
┌────────────────────────────────────────────────────┐
│  Worker class you write (wraps external event loop)  │
│  • Task queue (mutex + queue)                      │
│  • Wakeup mechanism (uv_async_send / eventfd / ...)│
│  • drain callback (execute fn(userdata) on loop)   │
│  • Thread management (start / stop)                │
└──────────────────────┬─────────────────────────────┘
                       │ dcb_foreign_register(name, schedule_fn, ctx)
                       ▼
┌────────────────────────────────────────────────────┐
│  ForeignExecutor (created internally by bridge)    │
│  schedule(Func) → box on heap → call your schedule_fn│
└──────────────────────┬─────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────┐
│  bridge channel system (co::oneshot / co::mpsc)    │
│  send() → wake_waiter → executor->schedule(resume)   │
└────────────────────────────────────────────────────┘
```

You only need to write the top layer. The ForeignExecutor and channel layers below it are already provided by the bridge.

## Integration Steps

### Worker Contract

Your Worker class only needs to satisfy one contract: **implement a schedule callback that guarantees `fn(userdata)` is executed on the loop thread**.

```c
// The bridge calls this callback to post a task. You must guarantee that fn(userdata) is eventually called on the loop thread.
typedef void (*dcb_schedule_fn)(void (*fn)(void*), void* userdata, void* ctx);
//                                            │              │           │
//                                    Function to run   Argument to fn   Context you passed at registration
//                                   (coroutine resume, etc.) (heap-allocated, freed inside fn) (e.g. UvWorker*)
```

Requirements:

* **Thread-safe**: the bridge may call this callback from any thread, so enqueueing must be under a lock
* **Non-blocking**: the callback itself only enqueues + wakes; it must not execute fn synchronously
* **Must execute**: every `fn(userdata)` must be called exactly once (otherwise the coroutine leaks)

Optionally, your Worker can also register **native timer callbacks** so that
`co_await async_simple::coro::sleep(...)` uses your event loop's own timer
instead of a waiter thread (see "Sleep and Cancellation on a ForeignExecutor"
below):

```c
// Schedule fn(userdata) on the loop thread after delay_us microseconds.
// Returns an opaque timer handle, or NULL on failure (bridge falls back to a
// waiter thread; on failure fn is NOT called and userdata must not be used).
typedef void* (*dcb_schedule_after_fn)(
    void (*fn)(void*), void* userdata, int64_t delay_us, void* ctx);

// Cancel a pending timer. Must be thread-safe and a safe no-op for handles
// that already fired or are unknown.
typedef void (*dcb_cancel_after_fn)(void* timer_handle, void* ctx);
```

### C API Provided by the Bridge

`#include "dart_cpp_bridge/foreign_runtime.h"`:

| Function | Purpose | When to call |
|------|------|----------|
| `dcb_foreign_register(name, schedule_fn, ctx)` | Register the runtime, returns runtime\_id | When the Worker starts |
| `dcb_foreign_register_ex(name, schedule_fn, schedule_after_fn, cancel_after_fn, ctx)` | Register with optional native timer support (pass NULL, NULL to get the same behavior as `dcb_foreign_register`) | When the Worker starts |
| `dcb_foreign_mark_loop_thread(id)` | Mark the current thread as the loop thread | After the loop thread starts, before running coroutines |
| `dcb_foreign_executor(id)` | Get the ForeignExecutor pointer | When you need `.via(ex)` or a channel |
| `dcb_foreign_unregister(id)` | Unregister (no more tasks will be received) | When the Worker stops |
| `dcb_post_to_bridge(fn, userdata)` | Post a task to the bridge io thread | When the external side needs to notify the bridge |

### Example: Complete UvWorker

```cpp
#include <uv.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include "dart_cpp_bridge/foreign_runtime.h"  // dcb_foreign_register_ex, etc.
#include "dart_cpp_bridge/foreign_executor.hpp"  // dcb::ForeignExecutor

class UvWorker {
 public:
  void start() {
    uv_loop_init(&loop_);

    // async handle: wake the loop thread across threads
    uv_async_init(&loop_, &async_, [](uv_async_t* h) {
      auto* self = static_cast<UvWorker*>(h->data);
      self->drain();  // Execute all pending tasks on the loop thread
    });
    async_.data = this;

    // Register with the bridge AND enable native timers, so
    // co_await sleep() uses uv_timer_t instead of a waiter thread per sleep.
    id_ = dcb_foreign_register_ex(
        "my-worker", &schedule_cb, &schedule_after_cb, &cancel_after_cb, this);

    // Start the loop thread
    thread_ = std::thread([this] {
      dcb_foreign_mark_loop_thread(id_);  // Mark current thread as the loop thread
      uv_run(&loop_, UV_RUN_DEFAULT);
    });
  }

  void stop() {
    dcb_foreign_unregister(id_);  // The bridge will no longer post tasks to us

    // Stop all pending timers on the loop thread before uv_run exits
    // (uv_loop_close fails if live handles remain). Wait for the cleanup:
    // uv_stop can make uv_run exit before the async wake-up is processed.
    {
      std::lock_guard lock(mu_);
      if (!live_timers_.empty()) {
        pending_.push({&stop_all_timers_task, this});
        stop_all_requested_.store(true, std::memory_order_release);
      }
    }
    if (stop_all_requested_.load(std::memory_order_acquire)) {
      uv_async_send(&async_);
      auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!stop_all_done_.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    uv_stop(&loop_);
    uv_async_send(&async_);       // Wake it so uv_run exits
    thread_.join();
    uv_loop_close(&loop_);
  }

  // Get the ForeignExecutor created by the bridge for this runtime (used for .via(ex) / channel coAwait)
  dcb::ForeignExecutor* executor() {
    return static_cast<dcb::ForeignExecutor*>(dcb_foreign_executor(id_));
  }

 private:
  // ─── The bridge calls this function to post tasks (may be called from any thread) ───
  static void schedule_cb(void (*fn)(void*), void* userdata, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    {
      std::lock_guard lock(self->mu_);
      self->pending_.push({fn, userdata});  // Enqueue under lock
    }
    uv_async_send(&self->async_);  // Thread-safely wake the loop
  }

  // ─── Native timer support (optional) ────────────────────────────────────
  //
  // schedule_after_cb is invoked on the loop thread (the awaiting coroutine
  // runs there), so uv_timer_init is safe. cancel_after_cb may be called from
  // ANY thread; it only enqueues a stop task and never touches the handle
  // outside the loop thread. live_timers_ (guarded by mu_) makes cancels a
  // safe no-op for timers that already fired.

  struct TimerBox {
    void (*fn)(void*);
    void* userdata;
    UvWorker* self;
  };
  struct CancelTask {
    UvWorker* self;
    uv_timer_t* timer;
  };

  static void* schedule_after_cb(void (*fn)(void*), void* userdata,
                                 int64_t delay_us, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    auto* timer = new uv_timer_t;
    if (uv_timer_init(&self->loop_, timer) != 0) {
      delete timer;
      return nullptr;  // Failure → bridge falls back to a waiter thread
    }
    auto* box = new TimerBox{fn, userdata, self};
    timer->data = box;
    const uint64_t timeout_ms =
        delay_us <= 0 ? 1 : static_cast<uint64_t>((delay_us + 999) / 1000);
    if (uv_timer_start(timer, &timer_cb, timeout_ms, 0) != 0) {
      self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
      uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
      return nullptr;
    }
    {
      std::lock_guard lock(self->mu_);
      self->live_timers_.insert(timer);
    }
    return timer;  // Opaque handle handed back to ForeignExecutor
  }

  static void timer_cb(uv_timer_t* timer) {
    auto* box = static_cast<TimerBox*>(timer->data);
    auto* self = box->self;
    {
      std::lock_guard lock(self->mu_);
      self->live_timers_.erase(timer);
    }
    box->fn(box->userdata);  // Resume the sleeping coroutine
    // Never delete a libuv handle directly: it stays in the loop's handle
    // queue until uv_close runs its close callback.
    self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
  }

  static void timer_close_cb(uv_handle_t* handle) {
    auto* timer = reinterpret_cast<uv_timer_t*>(handle);
    auto* box = static_cast<TimerBox*>(timer->data);
    auto* self = box->self;
    delete box;
    delete timer;
    const int remaining =
        self->pending_closes_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0 &&
        self->stop_all_requested_.load(std::memory_order_acquire)) {
      self->stop_all_done_.store(true, std::memory_order_release);
    }
  }

  static void cancel_after_cb(void* timer_handle, void* ctx) {
    auto* self = static_cast<UvWorker*>(ctx);
    auto* timer = static_cast<uv_timer_t*>(timer_handle);
    {
      std::lock_guard lock(self->mu_);
      if (self->live_timers_.find(timer) == self->live_timers_.end()) {
        return;  // Already fired or unknown: safe no-op
      }
      self->pending_.push({&cancel_timer_task, new CancelTask{self, timer}});
    }
    uv_async_send(&self->async_);  // libuv handles are only touched on the loop thread
  }

  static void cancel_timer_task(void* p) {
    auto task =
        std::unique_ptr<CancelTask>(static_cast<CancelTask*>(p));
    auto* self = task->self;
    auto* timer = task->timer;
    {
      std::lock_guard lock(self->mu_);
      if (self->live_timers_.erase(timer) == 0) {
        return;  // Timer callback already fired and closed it
      }
    }
    uv_timer_stop(timer);
    self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
    uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
  }

  static void stop_all_timers_task(void* p) {
    auto* self = static_cast<UvWorker*>(p);
    std::vector<uv_timer_t*> timers;
    {
      std::lock_guard lock(self->mu_);
      timers.assign(self->live_timers_.begin(), self->live_timers_.end());
      self->live_timers_.clear();
    }
    for (auto* timer : timers) {
      uv_timer_stop(timer);
      self->pending_closes_.fetch_add(1, std::memory_order_acq_rel);
      uv_close(reinterpret_cast<uv_handle_t*>(timer), &timer_close_cb);
    }
    // stop_all_done_ is set by timer_close_cb once every handle is closed.
  }

  // ─── Execute all pending tasks on the loop thread ───
  void drain() {
    std::queue<std::pair<void (*)(void*), void*>> batch;
    {
      std::lock_guard lock(mu_);
      batch.swap(pending_);  // Take everything at once to reduce lock time
    }
    while (!batch.empty()) {
      auto [fn, ud] = batch.front();
      batch.pop();
      fn(ud);  // Execute (e.g. coroutine resume, channel wakeup, etc.)
    }
  }

  uv_loop_t loop_{};
  uv_async_t async_{};
  std::mutex mu_;
  std::queue<std::pair<void (*)(void*), void*>> pending_;
  std::unordered_set<uv_timer_t*> live_timers_;
  std::thread thread_;
  uint32_t id_{0};
  std::atomic<int> pending_closes_{0};
  std::atomic<bool> stop_all_requested_{false};
  std::atomic<bool> stop_all_done_{false};
};
```

:::note
`dcb_foreign_executor()` returns `void*`, but the actual type is `dcb::ForeignExecutor*`; use `static_cast` when using it. The `executor()` method in the example already does this for you.

If your event loop has no timer facility, just pass `NULL, NULL` (or keep
using `dcb_foreign_register`); `co_await sleep()` then falls back to a
waiter thread and cancellation still works.
:::

### How It Works

```text
bridge any thread                    UvWorker                     libuv loop thread
───────────────────────────────────────────────────────────────────────────
ForeignExecutor::schedule(func)
  → box func on heap
  → call schedule_cb(fn, ud, ctx)
                              lock + push {fn, ud}
                              uv_async_send()
                                                      ──────▶  async callback fires
                                                               drain():
                                                                 fn(ud)
                                                                 → execute func
                                                                 → coroutine resume / channel wakeup
```

Key points:

* `schedule_cb` can be called from **any thread** (bridge io thread, other workers, etc.), so enqueueing must be under a lock
* `uv_async_send` is libuv's only thread-safe wakeup mechanism
* `drain()` is always executed on the loop thread, so `fn(ud)` needs no extra synchronization

### Timer Flow (co\_await sleep)

When the runtime registered `schedule_after_cb` / `cancel_after_cb`,
`co_await sleep()` is driven by `uv_timer_t`:

```text
awaiting coroutine (loop thread)         UvWorker                     libuv loop thread
───────────────────────────────────────────────────────────────────────────
co_await sleep(dur)
  ForeignExecutor::schedule(4-arg)
    → schedule_after_cb(delay_us)
                              new uv_timer_t + uv_timer_start
                              insert live_timers_
                              return handle
    → emplace Terminate handler on Slot
                                                      ──────▶  timer fires
                                                               timer_cb:
                                                                 erase live_timers_
                                                                 fn(ud) → resume coroutine
                                                                 uv_close(handle)

cancel path (any thread):
  signal->emits(Terminate)
    → Terminate handler
      → cancel_after_cb(handle)
                              lock; find in live_timers_
                              enqueue cancel task + uv_async_send
      → schedule_cb(resume)   ← handler also posts the resume
                                                      ──────▶  cancel_timer_task:
                                                               uv_timer_stop + uv_close
                                                               resume task:
                                                               TimeAwaiter throws SignalException
```

Key points for the timer callbacks:

* `schedule_after_cb` runs on the loop thread (the awaiting coroutine runs
  there), so initializing handles is safe; return `NULL` to fall back to the
  waiter thread.
* `cancel_after_cb` may run on **any** thread: never touch the libuv handle
  directly — enqueue a stop task and wake the loop. The `live_timers_` set
  makes cancels a safe no-op for timers that already fired.
* libuv handles must be released with `uv_close` (free in the close
  callback), never `delete` directly.
* On shutdown, close every live timer on the loop thread **before** `uv_stop`
  so `uv_loop_close` succeeds.

### Other Event Loops

| Runtime | Replace `uv_async_send` | Replace `drain` trigger point |
|---------|---------------------|--------------------|
| glib | `g_idle_add()` or `g_async_queue_push()` | idle callback |
| custom epoll | `eventfd` + write | after `epoll_wait` returns |
| Windows | `PostMessage()` | WndProc |

The pattern is the same in all cases: **lock and enqueue → wake loop → drain execution on the loop thread**.

## Calling Dart Callbacks (DartFn) from an External Runtime

### Method 1: Non-blocking (recommended)

The loop thread is **not blocked** while waiting for the Dart reply. After the coroutine suspends, it automatically resumes on the loop thread when Dart replies.

```cpp
// Global Worker (start() on app startup, stop() on exit)
static UvWorker g_worker;

// static coroutine function (required on MSVC, see the notes below)
static async_simple::coro::Lazy<> my_dart_fn_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  try {
    auto result = co_await cb(input);  // Non-blocking: suspend until Dart replies
    tx_ptr->send(std::move(result));
  } catch (const std::exception& e) {
    tx_ptr->send(std::string("ERROR: ") + e.what());
  }
  co_return;
}

// API function (runs on the bridge io thread):
async_simple::coro::Lazy<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  auto* ex = g_worker.executor();  // Get the ForeignExecutor
  auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

  // Start the coroutine on the external loop thread
  ex->schedule([tx_ptr, cb = std::move(callback), input = std::move(input), ex]() mutable {
    my_dart_fn_coro(std::move(tx_ptr), std::move(cb), std::move(input))
        .via(ex)
        .start([](auto&&) {});
  });

  // Bridge side suspends and waits for the result (does not block the io thread)
  auto reply = co_await rx.recv();
  if (!reply) throw std::runtime_error("worker dropped");
  co_return *reply;
}
```

**How it works:**

1. `ex->schedule(...)` posts the coroutine start to the loop thread
2. `.via(ex).start()` binds the coroutine to the ForeignExecutor and begins execution
3. Inside `co_await cb(input)`:
   * Creates a oneshot channel
   * Encodes arguments and sends a DartFnCall frame to Dart
   * `co_await rx.recv()` suspends the coroutine; the channel automatically captures the ForeignExecutor
4. After Dart executes the callback and replies → `complete_dart_fn` → `tx.send()`
5. `wake_waiter(h, ex)` → `ex->schedule(resume)` → the coroutine resumes on the loop thread
6. The result is passed back to the bridge main runtime through the outer oneshot channel

### Method 2: Blocking (simpler)

Block the current thread until Dart replies. Can be used on **any thread** (except the bridge io thread). No ForeignExecutor configuration needed.

```cpp
// On any worker thread (not the bridge io thread!)
auto result = async_simple::coro::syncAwait(dcb::spawn(cb(input)));
```

:::caution[Note]
* Using `syncAwait` on the loop thread blocks the entire event loop until Dart responds
* Using it on the bridge io thread causes a **self-deadlock** (Dart replies need the io thread to dispatch)
:::

### Comparison

| | Non-blocking | Blocking |
|--|---|---|
| Blocks the loop | No | Yes (until Dart replies) |
| ForeignExecutor setup | Full (virtual functions + mark\_loop\_thread) | None |
| MSVC workaround | Required (static coroutine function) | Not required |
| Typical use case | Production, concurrent work | Quick tests, simple scripts |

## Cross-Runtime Communication: Channel Service Pattern

The "Method 1" above is a one-shot interaction (post one task, wait for one result). If runtime A needs to **run long-term** and receive requests from multiple runtimes B/C/D, expose a "service" using an mpsc channel:

```text
Runtime B ───┐
              │  tx.send(request + reply channel)
Runtime C ───┼──────────────▶  Runtime A (mpsc receiver loop)
              │                    │
Runtime D ───┘                    │ After processing → reply_tx.send(result)
                                     ▼
                              B/C/D's oneshot rx receives the result
```

### Example: A Exposes a Service, B Calls It

```cpp
#include "dart_cpp_bridge/channel.hpp"

// ─── Request type: data + one-shot reply channel ───
struct Request {
  std::string payload;                              // Task data
  co::oneshot::Sender<std::string> reply_tx;        // A replies through this channel after processing
};

// ─── Runtime A: long-running service loop ───
// A exposes its sender to other runtimes and keeps its own receiver loop
class ServiceA {
 public:
  // Other runtimes send requests through this sender
  co::mpsc::Sender<Request> sender() { return tx_; }

  // Start the service loop on A's loop thread
  void run(dcb::ForeignExecutor* ex) {
    service_loop(std::move(rx_)).via(ex).start([](auto&&) {});
  }

 private:
  static async_simple::coro::Lazy<> service_loop(co::mpsc::Receiver<Request> rx) {
    while (auto req = co_await rx.recv()) {  // Suspend and wait for the next request
      // Process the task...
      std::string result = "processed: " + req->payload;
      // Reply through the one-shot channel given by B
      req->reply_tx.send(std::move(result));
    }
    co_return;  // Channel closed, service ends
  }

  co::mpsc::Sender<Request> tx_;
  co::mpsc::Receiver<Request> rx_;

 public:
  ServiceA() { auto [tx, rx] = co::mpsc::unbounded<Request>(); tx_ = std::move(tx); rx_ = std::move(rx); }
};

// ─── Runtime B: send a request and wait for the result non-blockingly ───
async_simple::coro::Lazy<std::string> call_service_a(
    co::mpsc::Sender<Request> a_sender, std::string data) {
  // Create a one-shot reply channel
  auto [reply_tx, reply_rx] = co::oneshot::channel<std::string>();

  // Send the request to A (non-blocking, can be called from any thread)
  a_sender.send(Request{std::move(data), std::move(reply_tx)});

  // Wait for A's reply non-blockingly (coroutine suspends, no thread is occupied)
  auto result = co_await reply_rx.recv();
  if (!result) throw std::runtime_error("service A dropped");
  co_return *result;
}
```

:::note[Implementation details]
* `Request` is **move-only** because it contains `co::oneshot::Sender`; `mpsc::send(Request{...})` moves it, it cannot be copied
* `ServiceA::run()` can only be called once: after `std::move(rx_)`, `rx_` is empty; calling it again starts a loop that immediately sees the channel closed
* If `Request` itself does not satisfy the underlying `moodycamel::ConcurrentQueue` constraints, wrap it in `std::shared_ptr<Request>` or `std::unique_ptr<Request>` before enqueueing
:::

### Differences from Method 1

| | Method 1 (schedule coroutine) | Channel service pattern |
|--|---|---|
| Number of interactions | One-shot (post one task, wait for one result) | Long-running (A continuously receives many requests) |
| Caller requirements | Must know A's executor | Only needs A's sender (thread-safe, can be sent from any thread) |
| Multiple callers | Must schedule each time | B/C/D share the same sender |
| Typical use case | Single cross-runtime task | Microservices/actor pattern, task queue |

Core principle: `tx.send()` is a thread-safe non-blocking operation that internally uses `wake_waiter` to schedule A's coroutine resume back onto A's executor. B's `co_await reply_rx.recv()` works the same way: the reply is automatically scheduled back onto B's executor. Different runtimes do not need to know each other's threading model; the channel handles cross-thread scheduling transparently.

## Using an Independent AsioExecutor Runtime

If your "external runtime" is also Asio-based (a separate `io_context` + `AsioExecutor` + thread), you do not need to implement the `schedule` callback yourself — `AsioExecutor` already fully implements all `async_simple::Executor` virtual functions. You can `co_await` DartFn directly in the coroutine:

```cpp
// WorkerRuntime owns an independent io_context + AsioExecutor + thread
// Note: On MSVC, coroutine lambdas also trigger the capture bug described below;
// for production code, write a static coroutine function; lambda is used here for brevity.
worker->spawn([cb = std::move(dartFn), input]() mutable -> async_simple::coro::Lazy<> {
  auto result = co_await cb(input);  // Directly co_await, no extra setup needed
  // Use result...
  co_return;
});
```

See `examples/multi_runtime_demo` for details.

## Sleep and Cancellation on a ForeignExecutor

`ForeignExecutor` overrides `schedule(Func, Duration, Slot*)` and picks between
two implementations:

* **Native timer (preferred)**: if the runtime registered
  `schedule_after_fn` / `cancel_after_fn` via `dcb_foreign_register_ex`, the
  sleep is driven by the event loop's own timer. A `SignalType::Terminate`
  handler stops the timer and posts the coroutine resume back to the loop, so
  cancellation is prompt and no thread is consumed per sleep.
* **Waiter-thread fallback**: when no timer callbacks are registered (or the
  native timer fails to start), a detached thread waits on a promise until the
  duration elapses **or** the `Terminate` signal fires, then posts the resume
  via `schedule_fn`. The thread captures shared state instead of `this`, so a
  pending sleep never dereferences the executor after it is deactivated or
  destroyed; once deactivated, pending waits post nothing and simply exit.

Cancellation works exactly like on `AsioExecutor` in both paths: bind a signal
with `Lazy::setLazyLocal` (or rely on `collectAll<Terminate>` /
`collectAny<Terminate>`), emit `SignalType::Terminate`, and the sleep throws
`async_simple::SignalException`.

`co_await sleep(...)` never blocks the loop thread in either path. The "do not
block the loop thread" rule still applies to *synchronous* code running
directly in a loop-thread callback (e.g. `std::this_thread::sleep_for` inside
a `schedule` lambda) — that still freezes the event loop.

The libuv demo (`examples/codegen_demo`) registers native timers via
`dcb_foreign_register_ex` and exercises both sleep and cancellation in
`test_foreign_sleep` / `test_foreign_sleep_cancel`.

## MSVC Notes

:::danger[MSVC 19.51 coroutine lambda capture bug]
On MSVC, variables captured by **coroutine lambdas** (`std::string`, `DartFn`, `shared_ptr`, etc.) become garbage after the coroutine resumes, causing an ACCESS\_VIOLATION crash.

You **must use a static coroutine function + parameter passing** instead of a coroutine lambda:

```cpp
// ✗ Crashes
auto lazy = [cb, input]() -> Lazy<> {
  co_await cb(input);  // cb/input are corrupted!
};

// ✓ Correct
static Lazy<> my_coro(DartFn<...> cb, std::string input) {
  co_await cb(input);  // Parameters are intact
}
```
:::

This bug is unrelated to ForeignExecutor and can be triggered by coroutine lambdas on any executor. See [async-simple Coroutine Basics](/dart_cpp_bridge/v1/guides/fundamentals/async-simple/) and [Known Issues §10](/dart_cpp_bridge/v1/reference/known-issues/).

## Key Design Constraints

| Constraint | Explanation |
|------|------|
| schedule must be thread-safe | The bridge may call it from any thread |
| fn(userdata) must execute on the loop thread | Prerequisite for correct coroutine resumption |
| Do not block the loop thread | **Synchronous** sleep / IO in a loop-thread callback freezes the event loop; `co_await coro::sleep(...)` is fine (native timer or waiter thread) |
| std::function must be copyable | Wrap move-only captures in `shared_ptr` |
| No more schedules after unregister | The executor becomes invalid after `dcb_foreign_unregister` |
| Fallback when executor is invalid | The channel's `wake_waiter` will inline resume to prevent coroutine leaks |

## Full Examples

* **libuv + ForeignExecutor**: `examples/foreign_runtime_demo`
* **Independent AsioExecutor runtime**: `examples/multi_runtime_demo`

## Don't Want C++ Coroutines?

If your code is pure C, or you do not want to introduce async-simple / asio dependencies, use the [Pure C Bridge API](/dart_cpp_bridge/v1/guides/advanced/cbridge/) — a zero-dependency callback-style interface for calling Dart functions or waiting for external async operations from any thread.
