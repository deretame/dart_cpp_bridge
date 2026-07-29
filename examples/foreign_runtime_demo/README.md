# Foreign Runtime Demo — External Runtime Integration

Demonstrates how to integrate a non-asio event loop (libuv in this case) with `dart_cpp_bridge`'s channel/coroutine system for non-blocking bidirectional cross-runtime communication.

## The Problem

The bridge's `co::oneshot` / `co::mpsc` channels rely on `async_simple::Executor` to wake waiting coroutines.
The only built-in implementation `AsioExecutor` is tightly coupled to `asio::io_context`, making it impossible for external event loops to participate directly.

## The Solution

Introduce a **ForeignExecutor** adapter layer + **C API registration mechanism**:

```
┌────────────────────────────────────────────────────┐
│  External Runtime (libuv / glib / custom loop)     │
│  Only needs: execute void(*)(void*) on loop thread │
└──────────────────────┬─────────────────────────────┘
                       │ dcb_foreign_register(name, schedule_fn, ctx)
                       ▼
┌────────────────────────────────────────────────────┐
│  ForeignExecutor (async_simple::Executor impl)     │
│  schedule(Func) → box to heap → call schedule_fn   │
└──────────────────────┬─────────────────────────────┘
                       │
                       ▼
┌────────────────────────────────────────────────────┐
│  bridge channel system (co::oneshot / co::mpsc)    │
│  send() → wake_waiter → executor->schedule(resume) │
└────────────────────────────────────────────────────┘
```

## Three Steps to Integrate

### 1. Implement the schedule callback

Your event loop only needs to provide a function that posts `void(*)(void*)` to the loop thread:

```cpp
// libuv example
static void schedule_callback(void (*fn)(void*), void* userdata, void* ctx) {
  auto* self = static_cast<UvWorker*>(ctx);
  self->push_task({fn, userdata});   // lock and enqueue
  uv_async_send(&self->async_);      // wake up loop
}
```

### 2. Register with the bridge

```cpp
uint32_t id = dcb_foreign_register("my-worker", &schedule_callback, this);
```

The returned `id` is used to get the executor or unregister later.

### 3. Bidirectional communication via channels

**Bridge → External Runtime** (bridge initiates request, external runtime processes and replies):

```cpp
auto [tx, rx] = co::oneshot::channel<std::string>();
auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

// Post to external loop thread for execution
executor()->schedule([tx_ptr, msg]() {
  std::string result = process(msg);   // runs on loop thread
  tx_ptr->send(std::move(result));     // non-blocking reply, wakes bridge coroutine
});

auto reply = co_await rx.recv();  // bridge side suspends (doesn't block io thread)
```

**External Runtime → Bridge** (external runtime pushes proactively):

```cpp
// On external loop thread
dcb_post_to_bridge(foreign_id_, [](void* ud) {
  // This code runs on bridge's asio io_context thread
  auto* data = static_cast<MyData*>(ud);
  // ... operate on bridge-side resources
}, data);
```

## Key Design Constraints

| Constraint | Description |
|------------|-------------|
| schedule must be thread-safe | bridge may call from any thread |
| fn(userdata) must run on loop thread | prerequisite for correct coroutine resumption |
| Don't block the loop thread | sleep / sync IO will stall the entire event loop |
| std::function requires copyable | wrap move-only types with `shared_ptr` |
| No schedule after unregister | executor becomes invalid after `dcb_foreign_unregister` |

## Demo Structure

```
examples/foreign_runtime_demo/
├── uv_worker.hpp              # libuv adapter (uv_async_t + task queue)
├── native/
│   ├── api/foreign_api.h      # BRIDGE_ASYNC annotated declarations
│   ├── api_impl/foreign_api.cpp  # business logic (channel communication)
│   └── generated/             # codegen output (do not edit)
├── lib/src/native_gen/        # codegen-generated Dart bindings
├── test/foreign_runtime_test.dart  # 11 tests
└── CMakeLists.txt             # FetchContent pulls libuv
```

## Build & Test

```bash
# 1. Ensure base library is built (provides asio/async-simple deps)
cmake -S ../../dart/native -B ../../dart/native/build
cmake --build ../../dart/native/build --config Release

# 2. Run codegen
cd ../../codegen
dart run bin/codegen.dart scripts/run_codegen.py ../examples/foreign_runtime_demo/dart_cpp_bridge.yaml

# 3. Build this demo
cd ../examples/foreign_runtime_demo
cmake -S native -B build
cmake --build build --config Release

# 4. Test
dart pub get
dart test
```

## Adapting Other Runtimes

Simply replace the event loop portion in `uv_worker.hpp`. For example, glib:

```cpp
static void schedule_callback(void (*fn)(void*), void* ud, void* ctx) {
  auto* self = static_cast<GlibWorker*>(ctx);
  // g_idle_add runs on main context thread
  g_idle_add([](gpointer p) -> gboolean {
    auto [f, u] = *static_cast<std::pair<void(*)(void*), void*>*>(p);
    f(u);
    return G_SOURCE_REMOVE;
  }, new std::pair{fn, ud});
}
```

The core remains unchanged: **implement `void(*)(void*)` execution on loop thread → register → channel communication**.

## Calling Dart Callbacks (DartFn) from External Runtimes

Once your external runtime is integrated via ForeignExecutor, you can call registered Dart callbacks from the loop thread. Two approaches are available:

### Non-blocking (recommended)

The loop thread is NOT blocked while waiting for Dart to reply. The coroutine suspends and is automatically resumed on the loop thread when Dart responds.

**Prerequisites:**
- ForeignExecutor must implement the full `async_simple::Executor` interface (`currentThreadInExecutor()`, `stat()`, `getIOExecutor()`)
- Call `dcb_foreign_mark_loop_thread(id)` from the loop thread at startup
- On MSVC: use a static coroutine function instead of a coroutine lambda (see [known_issues.md §10](../../docs/known_issues.md))

```cpp
// Static coroutine function (MSVC workaround — do NOT use coroutine lambda)
static async_simple::coro::Lazy<> my_dart_fn_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  try {
    auto result = co_await cb(input);  // non-blocking: suspends until Dart replies
    tx_ptr->send(std::move(result));
  } catch (const std::exception& e) {
    tx_ptr->send(std::string("ERROR: ") + e.what());
  }
  co_return;
}

// In your API function (runs on bridge io thread):
async_simple::coro::Lazy<std::string> call_dart_from_uv(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  auto* ex = worker->executor();
  auto tx_ptr = std::make_shared<co::oneshot::Sender<std::string>>(std::move(tx));

  // Schedule coroutine on the foreign loop thread
  ex->schedule([tx_ptr, cb = std::move(callback), input = std::move(input), ex]() mutable {
    my_dart_fn_coro(std::move(tx_ptr), std::move(cb), std::move(input))
        .via(ex)
        .start([](auto&&) {});
  });

  auto reply = co_await rx.recv();
  co_return *reply;
}
```

**How it works:**
1. `ex->schedule(...)` posts the coroutine launch to the loop thread
2. `.via(ex).start()` binds the coroutine to ForeignExecutor and begins execution
3. `co_await cb(input)` sends the request to Dart and suspends the coroutine
4. When Dart replies, `wake_waiter` schedules the resume back onto ForeignExecutor
5. The coroutine resumes on the loop thread with the result

### Blocking (simpler, but stalls the loop)

Blocks the calling thread until Dart replies. Works from ANY thread (except bridge's io thread — self-deadlock). No ForeignExecutor setup required.

```cpp
// On any worker thread (NOT bridge io thread!):
auto result = async_simple::coro::syncAwait(dcb::spawn(cb(input)));
```

> **Warning:** Using `syncAwait` on the loop thread blocks the entire event loop until Dart responds. Only use for quick callbacks or when the loop has no other work.

### Comparison

| | Non-blocking | Blocking |
|--|---|---|
| Loop blocked? | No | Yes (until Dart replies) |
| ForeignExecutor setup | Full (virtuals + mark_loop_thread) | None |
| MSVC workaround needed | Yes (static coro function) | No |
| Use case | Production, concurrent work | Quick tests, simple scripts |

---

## Performance & Optimization

### Current Lock Overhead

| Location | Purpose | Critical Section |
|----------|---------|------------------|
| `UvWorker::mu_` | task queue push/swap | nanoseconds (queue ops only) |
| `g_mu` | protect worker pointer | non-hot path (start/stop only) |
| `ForeignExecutor` | lock-free | `atomic<bool>` check |

For low-to-medium frequency messages (thousands msg/s), mutex is not a bottleneck.

### High-frequency Optimization (hundreds of thousands msg/s)

1. **Lock-free MPSC queue**: Replace `mutex + std::queue` with a lock-free linked list (e.g., intrusive MPSC) to eliminate lock contention on the schedule path.

2. **Batched wakeups**: Accumulate tasks before calling `uv_async_send` to reduce cross-thread wakeups:
   ```cpp
   // Pseudocode: wake every N tasks or every T microseconds
   if (pending_count++ % BATCH_SIZE == 0) uv_async_send(&async_);
   ```

3. **Avoid shared_ptr overhead**: Currently `shared_ptr` wraps Sender to satisfy `std::function` copyable requirement. Using move-only `unique_function` (C++23 `std::move_only_function` or custom) eliminates reference counting and heap allocation.

4. **Zero-copy transfer**: For large payloads, pass pointers/references instead of copying values, leveraging channel's move semantics.

### Design Trade-offs

This demo chooses mutex + `shared_ptr` because:
- Correctness first, code is clear and easy to understand
- The adapter's core goal is demonstrating the integration pattern
- Real bottlenecks are usually in business logic (e.g., IO in uv loop), not queue locks

For production use, simply replace the queue implementation inside `schedule_callback` — **the C API interface remains unchanged**.
