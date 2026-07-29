# Multi-Runtime Communication Demo (multi_runtime_demo)

Demonstrates how the `dart_cpp_bridge` main dispatcher and multiple independent C++ runtimes communicate via coroutine channels with **fully non-blocking** message passing.

## Architecture Overview

```text
┌─────────────────────────────────────────────────────────────────────┐
│  Dart Isolate                                                       │
│    await processMessage(message: "hello")                           │
│    workerStream(count: 5).listen(...)                               │
└──────────────────────────┬──────────────────────────────────────────┘
                           │ FFI (wire frames)
                           ▼
┌─────────────────────────────────────────────────────────────────────┐
│  Main Runtime — dcb::Runtime                                        │
│  asio::io_context (single-threaded) + AsioExecutor                  │
│                                                                     │
│  Role: receive Dart request → dispatch via channel → await → reply  │
└───────┬─────────────────────────────────────────────────┬───────────┘
        │ co::oneshot / co::mpsc                          │
        │ (send non-blocking, recv suspends coroutine)    │
        ▼                                                 ▼
┌───────────────────────────┐           ┌───────────────────────────┐
│  Worker A ("processor")   │           │  Worker B ("responder")   │
│  independent io_context   │           │  independent io_context   │
│  independent AsioExecutor │           │  independent AsioExecutor │
│  independent std::thread  │           │  independent std::thread  │
└───────────────────────────┘           └───────────────────────────┘
```

**Core Principle**: Each runtime owns its event loop and thread. Runtimes **share no threads**, **never block each other**. The only communication mechanism is channels.

## Communication: Coroutine Channels

This demo uses two channel types from `dart_cpp_bridge/channel.hpp`:

### oneshot — One-time Request/Reply

```cpp
#include "dart_cpp_bridge/channel.hpp"

// Create channel (can be used across threads)
auto [tx, rx] = co::oneshot::channel<std::string>();

// Sender (any thread, non-blocking)
tx.send("result");

// Receiver (inside coroutine, suspends instead of blocking thread)
auto reply = co_await rx.recv();  // std::optional<std::string>
```

Use case: one request, one reply (RPC pattern).

### mpsc — Multi-Producer Single-Consumer Stream

```cpp
auto [tx, rx] = co::mpsc::unbounded<std::string>();

// Sender (can send multiple times, non-blocking, returns false if receiver closed)
tx.send("item_0");
tx.send("item_1");
// tx destructor → channel closed → recv returns nullopt

// Receiver (consume in loop)
while (true) {
    auto item = co_await rx.recv();
    if (!item) break;  // channel closed
    // process *item
}
```

Use case: continuous data stream (Worker produces multiple items → Dart Stream).

### Key Properties

| Property | Description |
|----------|-------------|
| **send() never blocks** | Can be called from any thread, returns immediately |
| **recv() suspends coroutine** | Doesn't block the thread, only pauses current coroutine |
| **Thread-safe** | Internal mutex protection, tx/rx can move across threads |
| **Executor-aware** | When recv is woken, resumes on original runtime's thread via `ex->schedule()` |

## Demonstrated Communication Patterns

### 1. Single Worker Processing (oneshot)

```
Dart → Main → Worker A processes → oneshot reply → Main → Dart
```

```cpp
// Main side: create channel, dispatch task to Worker A
auto [tx, rx] = co::oneshot::channel<std::string>();
worker_a->spawn([tx = std::move(tx), msg]() mutable -> Lazy<> {
    tx.send("[A:" + msg + "]");  // runs on Worker A thread
    co_return;
});
// Main side: coroutine awaits reply (doesn't block Main thread)
auto reply = co_await rx.recv();
```

### 2. Pipeline Chained Processing (oneshot chain)

```
Dart → Main → Worker A → Worker B → Main → Dart
```

Workers also communicate via channels. Worker B's coroutine `co_await`s Worker A's output:

```cpp
auto [tx_ab, rx_ab] = co::oneshot::channel<std::string>();
auto [tx_final, rx_final] = co::oneshot::channel<std::string>();

// Worker A: process and send to Worker B
worker_a->spawn([tx_ab]() mutable -> Lazy<> {
    tx_ab.send("A{data}");
    co_return;
});

// Worker B: await A's result, then process
worker_b->spawn([rx_ab, tx_final]() mutable -> Lazy<> {
    auto from_a = co_await rx_ab.recv();  // suspend until A sends
    tx_final.send("B[" + *from_a + "]");
    co_return;
});

// Main: await final result
auto result = co_await rx_final.recv();  // "B[A{data}]"
```

### 3. Fan-out Parallel Dispatch

```
         ┌→ Worker A → reply_a ─┐
Dart → Main                      ├→ Main merges → Dart
         └→ Worker B → reply_b ─┘
```

Send to both Workers simultaneously, they execute in parallel, Main collects both replies:

```cpp
auto [tx_a, rx_a] = co::oneshot::channel<std::string>();
auto [tx_b, rx_b] = co::oneshot::channel<std::string>();
worker_a->spawn([tx_a]() -> Lazy<> { tx_a.send("A:msg"); co_return; });
worker_b->spawn([tx_b]() -> Lazy<> { tx_b.send("B:msg"); co_return; });

auto a = co_await rx_a.recv();  // both Workers execute in parallel
auto b = co_await rx_b.recv();
```

### 4. Worker Stream (mpsc)

```
Worker A produces continuously → mpsc channel → Main consumes → Dart Stream
```

```cpp
auto [tx, rx] = co::mpsc::unbounded<std::string>();

// Worker A: send continuously
worker_a->spawn([tx, count]() mutable -> Lazy<> {
    for (int i = 0; i < count; ++i) {
        tx.send("item_" + std::to_string(i));
    }
    // tx destructor → channel closed
    co_return;
});

// Main: consume and forward as Dart stream frames
while (true) {
    auto item = co_await rx.recv();
    if (!item) break;
    session->try_post(gen, make_frame(MsgType::kStreamData, ...));
}
session->try_post(gen, make_frame(MsgType::kStreamEnd, ...));
```

### 5. Calling Dart Callbacks from Worker Runtimes (DartFn)

```
Dart → Main → Worker A coroutine co_awaits DartFn → Dart processes → reply resumes on Worker A → Main → Dart
```

An independent runtime (using the library's `AsioExecutor`) can directly call registered Dart callbacks inside its coroutines — no extra configuration needed:

```cpp
#include "dart_cpp_bridge/dart_fn.hpp"

// API declaration: receives a Dart callback + input
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> call_dart_from_worker_a(
    dcb::DartFn<std::string(std::string)> callback, std::string input);

// Implementation: forward the DartFn to a Worker coroutine
async_simple::coro::Lazy<std::string> call_dart_from_worker_a(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();

  worker_a->spawn([tx = std::move(tx), cb = std::move(callback),
                   input = std::move(input)]() mutable -> Lazy<> {
    // co_await the Dart callback on Worker A's io thread
    auto result = co_await cb(input);  // non-blocking, coroutine suspends until Dart replies
    tx.send(std::move(result));
    co_return;
  });

  auto reply = co_await rx.recv();
  co_return *reply;
}
```

**How it works**:

1. `DartFn::operator()` internally creates a oneshot channel and `co_await rx.recv()`
2. The channel's `coAwait(Executor*)` automatically captures the current coroutine's executor (i.e., the Worker's `AsioExecutor`)
3. When Dart replies: `complete_dart_fn` → `tx.send(reply)` → `wake_waiter(h, worker_executor)`
4. The coroutine resumes on the Worker's io thread — no extra configuration required

**Notes**:

- The Worker's `AsioExecutor` must remain alive during the DartFn call (if the executor is deactivated, the coroutine falls back to inline resume on the sender's thread — no leak, but degraded scheduling)
- Multiple Workers can concurrently call different/same DartFns without interference
- Dart-side callback signature must be `Future<Ret> Function(Args...)` (async); synchronous returns are not supported

## WorkerRuntime Class

Each Worker is an independent runtime:

```cpp
class WorkerRuntime {
    asio::io_context ioc_;          // independent event loop
    dcb::AsioExecutor executor_;    // independent coroutine scheduler
    std::thread thread_;            // independent thread

    void start();                   // start event loop thread
    void stop();                    // stop and join

    // Spawn coroutine on this Worker's event loop
    template <class LazyFactory>
    void spawn(LazyFactory&& factory);
};
```

`spawn()` posts the coroutine to the Worker's own `io_context`. `co_await rx.recv()` inside the coroutine only suspends the coroutine, not the Worker thread.

## Why Non-Blocking?

| Operation | Behavior |
|-----------|----------|
| `tx.send()` | Returns immediately, puts value in channel's internal queue, wakes waiter |
| `co_await rx.recv()` | If no data, suspends current coroutine (thread continues processing other events) |
| Wakeup | Sender calls `executor->schedule(resume)` to post coroutine resumption back to receiver's event loop |
| Main thread | Never blocked by Workers; Workers never blocked by Main |

## Build & Run

```bash
# Prerequisite: build base library first (fetches asio / async-simple deps)
cmake -S ../../dart/native -B ../../dart/native/build
cmake --build ../../dart/native/build --config Release

# Run codegen to generate bindings (re-run after modifying native/api/*.h)
cd ../../codegen
dart run bin/codegen.dart scripts/run_codegen.py ../examples/multi_runtime_demo/dart_cpp_bridge.yaml
cd ../examples/multi_runtime_demo

# Build this demo
cmake -S native -B build
cmake --build build --config Release

# Run Dart tests
dart pub get
dart test
```

On Windows, if DLL auto-detection fails:

```powershell
$env:DCB_LIBRARY_PATH = "build\Release\dart_cpp_bridge.dll"
dart test
```

## File Structure

```text
multi_runtime_demo/
├── dart_cpp_bridge.yaml           # codegen config
├── worker_runtime.hpp             # WorkerRuntime class (independent event loop)
├── native/
│   ├── api/
│   │   └── multi_runtime_api.h    # BRIDGE_* annotated header (API definitions)
│   ├── api_impl/
│   │   └── multi_runtime_api.cpp  # business implementation (channel logic)
│   └── generated/                 # ← codegen auto-generated, do not edit
│       ├── wire_dispatch.hpp
│       ├── wire_dispatch.cpp
│       └── ir.json
├── CMakeLists.txt                 # build config
├── pubspec.yaml                   # Dart package definition
├── lib/
│   ├── multi_runtime_demo.dart    # package entry (exports generated code)
│   └── src/native_gen/            # ← codegen auto-generated Dart bindings
│       ├── dcb_generated.dart     #   BridgeApiImpl singleton
│       └── api/
│           ├── init.dart          #   DcbLib initialization class
│           └── multi_runtime_api.dart  # top-level function API
└── test/
    ├── multi_runtime_test.dart    # 19 integration tests
    └── support/library_path.dart  # DLL path resolution
```

## Design Highlights

1. **Fully Independent Runtimes**: Each Worker owns its `io_context` + thread, sharing no scheduling state.
2. **Channel is the Only Communication**: No shared memory, no callbacks, no lock contention (except Worker lifecycle mutex).
3. **Coroutine Suspension ≠ Thread Blocking**: `co_await` only pauses the coroutine; the event loop continues processing other tasks.
4. **Executor-Aware Resumption**: When channel recv is woken, it returns to the original runtime's thread via `schedule()`, avoiding cross-thread coroutine resumption.
5. **Seamless Dart Integration**: Dart side only sees `Future<T>` and `Stream<T>`; the underlying cross-runtime communication is completely transparent.
6. **Codegen Auto-Generates Bindings**: Just declare APIs with `BRIDGE_ASYNC` / `StreamSink` in `native/api/*.h`; codegen auto-generates C++ wire dispatch and Dart bindings. Business code only writes pure `Lazy<T>` coroutines.

## Performance & Optimization

### Current Lock Overhead

| Location | Purpose | Critical Section |
|----------|---------|------------------|
| `channel.hpp` internal mutex | protect channel state (sender/receiver matching) | nanoseconds (pointer swap) |
| `WorkerRuntime` lifecycle mutex | start/stop protection | non-hot path |
| `asio::post` | internal lock-free queue (io_context built-in) | — |

Channel's `send()` internally only does one short mutex (match waiter or enqueue). For low-to-medium frequency messages (thousands msg/s), this is not a bottleneck.

### High-Frequency Optimization (hundreds of thousands msg/s)

1. **Lock-free channel**: Replace channel's internal `mutex + optional` with `atomic` CAS operations (e.g., `std::atomic<Waiter*>` lock-free linked list) to eliminate locks on send/recv path.

2. **Batched posting**: Merge multiple `asio::post` calls into one wakeup. asio has internal optimizations, but business layer can batch further:
   ```cpp
   // Pseudocode: collect multiple results then post once
   std::vector<Result> batch;
   for (auto& item : items) batch.push_back(process(item));
   asio::post(ioc, [batch = std::move(batch)] { deliver(batch); });
   ```

3. **Avoid shared_ptr overhead**: Currently `shared_ptr` wraps move-only Sender when crossing `std::function` boundary. Using `std::move_only_function` (C++23) or custom `unique_function` eliminates reference counting.

4. **Memory pool**: For high-frequency channel creation/destruction, use object pools to reuse channel internal nodes, reducing malloc/free.

5. **Multi-consumer extension**: Current mpsc is single-consumer. For multiple Workers consuming in parallel, extend to work-stealing mode or introduce `co::mpmc`.

### Design Trade-offs

This demo chooses mutex-based channel because:
- Correctness first, implementation is simple and auditable
- Core goal is demonstrating cross-runtime communication patterns
- Real bottlenecks are usually in business computation or IO, not channel locks

For production optimization, simply replace `channel.hpp` internal implementation — **the external send/recv interface remains unchanged**, business code needs no modification.
