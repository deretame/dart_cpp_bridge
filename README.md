# dart_cpp_bridge

**English** | [中文](README.zh-CN.md)

A **C++20** interoperability bridge for **Dart / Flutter**: connect existing C/C++ code to Dart with coroutines and an event loop, aiming for an experience close to [Flutter Rust Bridge (FRB)](https://cjycode.com/flutter_rust_bridge/).

Repository: <https://github.com/deretame/dart_cpp_bridge>

> **Status**: Experimental standalone repo. Phase 1 hand-written runtime is largely done. Phase 2 codegen can generate **SYNC/ASYNC/NORMAL** for a fixture (`examples/codegen_demo`). **Native Assets / rich types / production template** are not ready — not a drop-in FRB replacement yet.

---

## Why this project?

In the ecosystem today:

- **Rust** has a mature [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/) with a clear path from business functions to Dart APIs;
- **C / C++** still powers a huge amount of existing code (media, networking, games, embedded, legacy business libs…), but there is no equally ergonomic Dart integration story.

Typical options are hand-rolled FFI plus homemade threading/callbacks, or fragmented JNI / platform channels. Async and streaming often turn into callback hell or a blocked event loop.

**This project aims to fill that gap:**

- Give C++ code a **clear integration surface** (sync / async / stream / reverse Dart closures);
- Write async logic with **C++20 coroutines**, semantically close to Dart `async` / `await` / `Stream`;
- Follow FRB-like runtime ideas (process-wide Runtime, per-Isolate Session, long-lived port + `request_id`) to reduce mental overhead.

---

## Design principles

| Principle | Notes |
|-----------|--------|
| FRB-aligned UX | Channels, lifecycle, and DartFn reverse calls mirror FRB concepts for easy comparison |
| Coroutines first | Business code uses `async_simple::coro::Lazy<T>` and `co_await` on the io thread — not `std::future::get` everywhere |
| No babysitting misuse | e.g. `syncAwait` on the io thread is a self-deadlock — documented; caller owns the risk |
| Incremental delivery | Phase 1 hand-written demo → Phase 2 codegen → Phase 3 Native Assets / productization |

Business code does **not** return a bridge-specific Future wrapper. Write normal sync functions or `Lazy`; the bridge handles codec and scheduling.

---

## Tech stack

| Layer | Choice | Role |
|-------|--------|------|
| Language | **C++20** (minimum) | Coroutines, concepts, etc. Use a C++20-capable compiler (recent MSVC / GCC / Clang) |
| Event loop | **[Asio](https://think-async.com/Asio/)** (standalone) | Single-threaded `io_context`: timers, post, external completion |
| Coroutines & channels | **[async-simple](https://github.com/alibaba/async-simple)** | `Lazy`, `Executor`; plus this repo’s `co::oneshot` / mpsc-style channels |
| Dart side | Dart 3 + `package:ffi` | Isolates, ReceivePort, Completer / Stream |
| Reference | Flutter Rust Bridge | Architecture & API shape — not a code dependency |

Runtime (simplified):

```text
┌─────────────────────────────────────────────────────────┐
│  Dart Isolate(s)                                        │
│    Session (one reply port per Isolate)                 │
│    Future / Stream / DartFn callbacks                   │
└──────────────────────────▲──────────────────────────────┘
                           │ FFI + binary frames
┌──────────────────────────┴──────────────────────────────┐
│  Runtime (process-wide)                                 │
│    asio::io_context (single-threaded) + AsioExecutor    │
│    asio::thread_pool (normal / blocking work)           │
│    wire: sync / async Lazy / stream / DartFn            │
└─────────────────────────────────────────────────────────┘
```

**DartFn (C++ → Dart closure) async path**: true `co_await` on oneshot on the io thread — **does not block io, does not occupy a pool thread**. Sync path blocks the current thread (blocking io is the caller’s problem).

---

## Current capabilities (Phase 1)

| Capability | Status |
|------------|--------|
| Single-threaded asio + thread_pool | ✅ |
| Process-wide Runtime / per-Isolate Session | ✅ |
| sync / async (`Lazy`) / normal / stream | ✅ |
| DartFn reverse calls (parameter-style closures, FRB-like) | ✅ (async = true io suspend) |
| NativeFinalizer auto session close | ✅ |
| Dart tests (incl. multi-Isolate) | ✅ |
| C++ smoke (oneshot / DartFn e2e) | ✅ |
| Codegen (scan + markers → wire / Dart) | ⏳ SYNC/ASYNC/NORMAL + Dart 3-layer API; see `examples/codegen_demo` |
| Native Assets hook productization | ⏳ not started |

### Demo API (hand-written)

| C++ / concept | Dart | Channel |
|---------------|------|---------|
| `bridgeVersion` | `int bridgeVersion()` | sync |
| `add(a, b)` | `Future<int> add(a, b)` | async (`Lazy`) |
| `sleepTest` | `Future<String> sleepTest()` | normal (pool) |
| `ticks(count, intervalMs)` | `Stream<int> ticks(...)` | stream |
| `callDartHello(cb)` | `Future<String> callDartHello(cb)` | DartFn **async** (io `co_await`) |
| `callDartHelloSync(cb)` | `Future<String> callDartHelloSync(cb)` | DartFn **sync** (blocks current thread) |

---

## C++ coroutine launch API (`spawn` family)

`dart_cpp_bridge/runtime.hpp` provides three helpers for launching `async_simple::coro::Lazy<T>` from **non-coroutine** code. Inside a coroutine running on io, none of them are needed — just `co_await std::move(lazy)` directly (it inherits the current executor and rethrows exceptions).

| Helper | Returns | Purpose |
|--------|---------|---------|
| `dcb::spawn(lazy)` | `RescheduleLazy<T>` (lazy, not started) | Bind the coroutine to the io executor; the caller chooses how to trigger it |
| `dcb::spawn_detached(lazy)` | `void` | Fire-and-forget: starts immediately on io, discards value **and** exception |
| `dcb::spawn_blocking(f)` | `Lazy<T>` | Run a blocking callable on the thread pool; awaiting it never blocks io |

### `spawn` — three ways to trigger

`spawn` returns a `RescheduleLazy` that is bound to io but **not yet started** (it is `[[nodiscard]]` and lazy — dropping it means the coroutine never runs). Pick one:

```cpp
#include <async_simple/coro/SyncAwait.h>

// 1) Block a normal (NON-io) thread until ready; returns the value or
//    rethrows the coroutine's exception. No std::promise/future plumbing.
int v = async_simple::coro::syncAwait(dcb::spawn(std::move(lazy)));

// 2) Fire-and-forget (discard value and exception):
dcb::spawn_detached(std::move(lazy));

// 3) Custom completion callback (Try<T> carries value-or-exception):
dcb::spawn(std::move(lazy)).start([](async_simple::Try<int>&& t) { /* ... */ });
```

### `spawn_blocking` — offload blocking work

```cpp
// inside a coroutine on io — io thread keeps running while f() blocks a pool thread:
auto v = co_await dcb::spawn_blocking([&] { return heavyComputation(); });

// block a normal thread for the result:
auto v = async_simple::coro::syncAwait(dcb::spawn(dcb::spawn_blocking(f)));

// fire-and-forget from non-coroutine code:
dcb::spawn_detached(dcb::spawn_blocking(f));
```

Exceptions thrown by the callable are captured on the pool thread (`Promise::setException`) and rethrown at the awaiter. Void callables are supported (bridged through `async_simple::Unit` so exceptions still propagate — a bare `co_await Future<void>` would silently drop them).

### Timer / sleep

For delays inside a coroutine, use the standard async_simple API — no bridge wrapper needed:

```cpp
#include <async_simple/coro/Sleep.h>

co_await async_simple::coro::sleep(std::chrono::milliseconds(100));
```

This suspends on an `asio::steady_timer` on the io event loop (via `AsioExecutor`'s timed `schedule` override): the io thread stays responsive while waiting and no pool/OS thread is occupied — so don't hand-roll a `Promise` + timer wrapper.

### Safety constraints

- **Never `syncAwait` on the io thread.** It blocks its caller, and the awaited Lazy needs the io thread to make progress — self-deadlock. `AsioExecutor::currentThreadInExecutor()` identifies the io thread, and `syncAwait` refuses with an assertion there. From io, `co_await` instead.
- **Do not use async_simple's `RescheduleLazy::detach()`.** Its completion callback rethrows the coroutine's exception *on the io thread*, which escapes into the event loop and terminates it. Use `dcb::spawn_detached` (empty callback) instead.
- **Keep coroutine lambdas alive.** A coroutine lambda's captures live in the lambda object, not the coroutine frame. `Runtime::spawn_on_asio` keeps the factory alive via a shared holder; with raw `spawn(...).start(...)` prefer free coroutine functions (parameters are copied into the frame) or ensure captures outlive the coroutine.

---

## Quick start

### Requirements

- CMake ≥ 3.24  
- **C++20** compiler  
- Git (FetchContent pulls Asio / async-simple)  
- **Dart SDK ≥ 3.10** (`dart/pubspec.yaml`: `sdk: ^3.10.0`)  
  - Build hooks / Native Assets: ≥ 3.10 (stable Flutter with Dart 3.12.x is fine today)  
  - Link hooks + recorded-usage tree-shaking: ≥ 3.13 (raise the floor once stable is widespread)

### Build C++

```bash
# 1) Fetch Dart API DL headers
cmake -P cmake/fetch_dart_api.cmake

# 2) Configure & build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 3) Native smoke
./build/dcb_smoke                 # Linux / macOS (path varies by generator)
build/Release/dcb_smoke.exe       # Windows multi-config generators
```

Windows (PowerShell):

```powershell
cmake -P cmake/fetch_dart_api.cmake
cmake -S . -B build
cmake --build build --config Release
.\build\Release\dcb_smoke.exe
```

### Dart side

```powershell
cd dart
dart pub get
```

```dart
import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

Future<void> main() async {
  final b = await DartCppBridge.init(
    libraryPath: r'..\build\Release\dart_cpp_bridge.dll', // adjust per platform
  );

  print(b.bridgeVersion());
  print(await b.add(40, 2));
  print(await b.sleepTest());

  // DartFn: C++ awaits on io; Dart runs the closure
  print(await b.callDartHello((name) => 'Hello, $name!'));
  print(await b.callDartHello((name) async {
    await Future<void>.delayed(const Duration(milliseconds: 10));
    return 'Hello async, $name!';
  }));

  await for (final n in b.ticks(count: 3, intervalMs: 0)) {
    print('tick $n');
  }

  // Before process exit (main isolate only)
  b.shutdown();
}
```

### Tests

```powershell
# C++ smoke (no Dart VM; simulates post / DartFn reply)
.\build\Release\dcb_smoke.exe

# Full Dart FFI suite (build the dynamic library first)
cd dart
dart test
```

By default tests load `build/Release/dart_cpp_bridge.dll` (or the platform so/dylib). Override with:

```powershell
$env:DCB_LIBRARY_PATH = "D:\path\to\dart_cpp_bridge.dll"
dart test
```

---

## Lifecycle (FRB-style)

| Action | Who | Notes |
|--------|-----|--------|
| `DartCppBridge.init` | Every Isolate that uses the bridge | Opens this isolate’s Session + reply port |
| `dispose` | Optional | Closes session immediately; **usually not required** |
| `NativeFinalizer` | Automatic | `session_close` when unreachable / isolate shuts down |
| `shutdown` | **Main isolate on process exit only** | Stops process-wide Runtime; workers must not call this |

Model: **one Runtime per process; one Session per Isolate** — background isolates can use async/stream independently.

---

## Repository layout

```text
docs/
  frb_and_cpp_bridge_design.md   # full design
  progress.md                    # implementation progress
  known_issues.md                # tech debt / resolved issues
include/dart_cpp_bridge/         # public C++ headers
src/                             # runtime · wire · ffi_entry
third_party/dart_api/            # Dart API DL
dart/                            # Dart package + tests
examples/phase1_demo/            # C++ smoke
cmake/                           # helper scripts
dcb_gen_tool/                    # CLI tool + parse/generate
examples/codegen_demo/           # codegen fixture (yaml + generated + tests)
hook/                            # Native Assets placeholder
```

---

## Documentation

| Doc | Content |
|-----|---------|
| [docs/frb_and_cpp_bridge_design.md](docs/frb_and_cpp_bridge_design.md) | Design decisions, channel model, FRB comparison |
| [docs/progress.md](docs/progress.md) | Progress and landed checklist |
| [docs/known_issues.md](docs/known_issues.md) | Known / resolved issues (incl. DartFn oneshot) |

Design and progress docs are currently written primarily in **Chinese**; English summaries live in this README and [README.zh-CN.md](README.zh-CN.md).

---

## Roadmap (summary)

1. **Phase 1** (current): Hand-written skeleton — Runtime / Session / four channels / DartFn  
2. **Phase 2**: Codegen MVP done for primitive SYNC/ASYNC/NORMAL → extend structs/Stream/DartFn + templates  
   Docs: [`dcb_gen_tool/README.md`](dcb_gen_tool/README.md), [`examples/codegen_demo`](examples/codegen_demo/)
3. **Phase 3**: Native Assets hooks, CMake export, polished examples  
4. **Phase 4**: Real app integration (does not replace existing FRB bridges by default)

---

## Non-goals (for now)

- No ABI / API stability guarantees  
- No hidden magic that offloads `syncAwait` if you block the io thread  
- No shipping C++ stdlib or business binaries inside the pub package (hooks will own compile/link later)  
- Not a UI platform-channel replacement — **Dart ↔ native logic** only  

---

## Acknowledgments

- [Flutter Rust Bridge](https://github.com/fzyzcjy/flutter_rust_bridge) — architecture and product shape  
- [Asio](https://think-async.com/Asio/) — event loop and async I/O  
- [async-simple](https://github.com/alibaba/async-simple) — C++20 coroutine runtime  
- Dart / Flutter teams — FFI, Isolates, NativeFinalizer, and more  

---

## License

This project is licensed under the **MIT License** — see [LICENSE](LICENSE).

Asio, async-simple, Dart SDK headers, and other third-party components remain under their own licenses.

---

## Contributing

Please read `docs/` first for design and open issues. Issues and PRs welcome for Phase 1 behavior, tests, and docs:  
<https://github.com/deretame/dart_cpp_bridge/issues>
