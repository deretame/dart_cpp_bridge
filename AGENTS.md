# AGENTS.md — dart_cpp_bridge

> Quick reference for AI coding agents working on `dart_cpp_bridge`.
> Read this if you know nothing about the project. For deep design context, see `docs/frb_and_cpp_bridge_design.md` (Chinese) and `README.md` (English).

## Project overview

`dart_cpp_bridge` is an experimental **Dart ↔ C++20** interoperability bridge inspired by [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/). It lets existing C/C++ code expose sync, async, stream, and reverse Dart-closure APIs to Dart/Flutter with a runtime that feels like FRB.

- **Status**: Experimental / Phase 1 + early Phase 2. Not a production drop-in replacement for FRB.
- **Goal**: Give C++ libraries a clean integration surface (sync / async / stream / DartFn reverse calls) using C++20 coroutines and a single-threaded Asio event loop.
- **Repository**: <https://github.com/deretame/dart_cpp_bridge>

### High-level architecture

```text
Dart Isolate(s)
  Session per Isolate (one long-lived reply port)
  Future / Stream / DartFn callbacks
       ⇅  FFI binary frames
Runtime (process-wide)
  asio::io_context (single-threaded) + AsioExecutor
  asio::thread_pool (blocking / normal work)
  wire: sync / async Lazy / stream / DartFn
```

Core principle: **business C++ code is written as normal functions or `async_simple::coro::Lazy<T>`; the bridge handles codec, scheduling, and Dart API generation.** Do not invent bridge-specific Future/Stream wrapper types in business code.

## Technology stack

| Layer | Technology | Notes |
|-------|------------|-------|
| C++ standard | C++20 minimum | Coroutines, concepts. Requires recent MSVC/GCC/Clang. |
| Event loop | [Asio](https://think-async.com/Asio/) standalone | `asio::io_context` single-threaded; timers, post, completion. |
| Coroutines | [async-simple](https://github.com/alibaba/async-simple) | `Lazy`, `Executor`. Header-only use for `coro`. |
| Dart side | Dart 3 + `package:ffi` | Isolates, `ReceivePort`, `Completer`, `Stream`, `NativeFinalizer`. |
| Dart SDK | `>= 3.10.0` | `dart/pubspec.yaml` floor. Native Assets hooks need 3.10+; link hooks need 3.13+. |
| Codegen | Pinned Python 3.13.13 + libclang-ng 22.1.4.2 | Downloaded from remote, cached, hash-verified. No host Python/LLVM. |
| Build | CMake 3.20+ | FetchContent pulls Asio/async-simple. Native Assets hook is not yet wired. |

## Directory structure

```text
.
├── CMakeLists.txt              # Main C++ shared library (dart_cpp_bridge)
├── cmake/fetch_dart_api.cmake  # Download Dart API DL headers
├── README.md / README.zh-CN.md
├── docs/
│   ├── frb_and_cpp_bridge_design.md   # Design decisions (Chinese)
│   ├── progress.md                    # Implementation progress
│   └── known_issues.md                # Resolved/known tech debt
├── include/dart_cpp_bridge/   # Public C++ headers
│   ├── runtime.hpp            # Singleton Runtime, spawn_on_asio
│   ├── session.hpp            # Session, SessionRegistry, DartFnReply
│   ├── channel.hpp            # co::mpsc / co::oneshot coroutine channels
│   ├── dart_fn.hpp            # DartFnStringToString (sync/async reverse call)
│   ├── stream_sink.hpp        # StreamSink<T>
│   ├── codec.hpp              # Wire frame + ByteReader/Writer
│   ├── ffi.h                  # C ABI exported by the shared library
│   ├── asio_executor.hpp      # AsioExecutor for async-simple
│   └── annotate.h             # BRIDGE_* / DCB_* codegen markers
├── src/
│   ├── runtime/runtime.cpp    # Runtime impl, Session impl, DartFn invoke
│   ├── wire/demo_api.cpp      # Hand-written demo wire dispatch
│   └── ffi_entry.cpp          # C ABI exports (dcb_init_dart_api, etc.)
├── dart/                      # Dart package (pub package root)
│   ├── pubspec.yaml
│   ├── lib/                   # dart_cpp_bridge package
│   │   ├── src/bridge.dart    # DartCppBridge class
│   │   ├── src/bindings.dart  # FFI bindings
│   │   ├── src/codec.dart     # Dart codec mirror
│   │   └── dart_cpp_bridge.dart
│   ├── test/                  # FFI + codec tests
│   └── example/example.dart
├── codegen/                   # Codegen toolchain (manual, not in build hook)
│   ├── versions.lock          # Pinned Python + libclang-ng URLs/hashes
│   ├── bootstrap.ps1/.sh      # Download/verify toolchain into user cache
│   ├── codegen.ps1/.sh        # Entry: bootstrap + run Python script
│   ├── scripts/               # parse/generate Python scripts
│   └── stubs/                 # Stub headers for codegen parsing
├── third_party/dart_api/      # Dart API DL C headers (downloaded)
└── examples/
    ├── phase1_demo/           # C++ smoke test (no Dart VM)
    │   └── smoke_main.cpp
    └── codegen_demo/          # Phase 2 fixture
        ├── dart_cpp_bridge.yaml
        ├── native/api/bridge_api.h
        ├── native/api_impl/bridge_api.cpp
        ├── native/generated/  # Generated wire_dispatch.* + ir.json
        ├── lib/             # Generated Dart API + manual export
        ├── test/
        └── CMakeLists.txt
```

## Build commands

### Requirements

- CMake >= 3.20
- C++20 compiler (MSVC 2019+, GCC 10+, Clang 12+)
- Dart SDK >= 3.10.0
- Git (for FetchContent)
- Network for first C++ build (Asio/async-simple) and codegen toolchain

### C++ main library

```bash
# 1. Fetch Dart API DL headers (one-time unless deleted)
cmake -P cmake/fetch_dart_api.cmake

# 2. Configure & build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 3. Smoke test (no Dart VM)
./build/dcb_smoke                 # Linux/macOS path varies
./build/Release/dcb_smoke.exe       # Windows multi-config
```

### Dart package

```bash
cd dart
dart pub get

# Build the C++ library first, then:
dart test
```

Override native library path:

```bash
# PowerShell
$env:DCB_LIBRARY_PATH = "D:\path\to\dart_cpp_bridge.dll"
dart test

# Bash
DCB_LIBRARY_PATH=/path/to/libdart_cpp_bridge.so dart test
```

### Codegen demo fixture

```bash
# 1. Build main library first (reuses _deps for asio/async-simple)
cmake -S . -B build
cmake --build build --config Release

# 2. Run codegen for the demo fixture
cd codegen
./codegen.sh scripts/run_codegen.py ../examples/codegen_demo/dart_cpp_bridge.yaml
# or on Windows:
# .\codegen.ps1 -Script scripts/run_codegen.py -- ..\examples\codegen_demo\dart_cpp_bridge.yaml

# 3. Build the demo library
cd ../examples/codegen_demo
cmake -S . -B build
cmake --build build --config Release

# 4. Run Dart tests
dart pub get
dart test
```

## Code organization and module divisions

### C++ side

- **Public headers** (`include/dart_cpp_bridge/`): the runtime surface. Business code and generated wire include these.
- **Runtime** (`src/runtime/runtime.cpp`): process-wide `Runtime`, per-isolate `Session`, `SessionRegistry`, `DartFnStringToString` async implementation.
- **Wire** (`src/wire/demo_api.cpp` or generated `wire_dispatch.cpp`): frame dispatch, method routing, codec, scheduling. This is the only place that knows about `request_id`, `method_id`, and port posting.
- **FFI entry** (`src/ffi_entry.cpp`): C ABI exports used by Dart. This is the dynamic-library boundary.

### Dart side

- `dart/lib/src/bridge.dart`: high-level `DartCppBridge` class — session lifecycle, `invokeSyncMethod`, `invokeAsyncMethod`, `ticks`, `callDartHello`, etc.
- `dart/lib/src/bindings.dart`: raw FFI bindings to `dcb_*` C functions.
- `dart/lib/src/codec.dart`: frame encoding/decoding mirror of `include/dart_cpp_bridge/codec.hpp`.

### Generated code (Phase 2 codegen)

For a user project, codegen emits three layers in Dart:

```text
api_fn.dart   # top-level functions: initBridge(), add(), ...  (preferred call site)
api.dart      # BridgeApi.instance singleton (init / dispose / forward)
api.g.dart    # BridgeApiImpl: method ids, codec, invoke* calls
```

C++ side emits `native/generated/wire_dispatch.hpp|.cpp` and `ir.json`. Business implementation stays in user-written files like `native/api_impl/bridge_api.cpp`.

## Wire protocol

Little-endian binary frame (`include/dart_cpp_bridge/codec.hpp` and `dart/lib/src/codec.dart`):

```text
magic       u32   0x31424344 ('DCB1')
version     u16   1
msg_type    u8    request / responseOk / responseErr / streamData / streamEnd / streamErr / dartFnCall
flags       u8    reserved 0
request_id  u64   RPC id, stream id, or DartFn reply id
method_id   u32   generated or hand-written method id
payload_len u32
payload     bytes
```

Errors are always encoded as frames with `msg_type=responseErr` and payload `code i32 + message string`. C++ exceptions are caught at the wire boundary and never cross FFI.

## Lifecycle rules

- **Runtime**: process-wide singleton. Started on first `DartCppBridge.init()`; stopped by `shutdown()`.
- **Session**: one per Isolate that calls `init()`. Each session has its own reply port.
- **NativeFinalizer**: automatically closes the native session when the Dart `DartCppBridge` object becomes unreachable or the isolate shuts down. Manual `dispose()` is optional.
- **shutdown()**: closes all sessions and stops the runtime. **Only call from the main isolate on process exit.** Never from worker isolates.
- **dispose()**: closes this isolate's session immediately. Optional in normal apps.

## Code style guidelines

- **C++**: C++20, no extensions, no RTTI/exception changes beyond standard C++ exceptions. Use `std::` consistently.
- **Coroutines**: prefer `async_simple::coro::Lazy<T>` for async business code. Do not write custom `bridge::Future<T>` wrappers.
- **Threading**: never block the `io_context` thread. Blocking work goes to `asio::thread_pool` (normal/stream kind) or `spawn_blocking`.
- **Error handling**: at the wire boundary always catch `const std::exception&` first, then `(...)`. Encode errors into frames; do not let exceptions propagate across FFI.
- **Dart**: follows standard Dart package conventions, `package:lints` for static analysis. Use `final` and `StateError` for runtime failures.
- **Naming**: C++ namespace `dcb`, generated wire namespace `dcb::demo`. Dart classes use `PascalCase`.
- **Codegen markers**: in user headers use `BRIDGE_SYNC`, `BRIDGE_ASYNC`, `BRIDGE_NORMAL`, or `BRIDGE_EXPORT` (also aliased as `DCB_*`). They expand to `__attribute__((annotate("bridge::*")))` only when `BRIDGE_CODEGEN` / `DART_CPP_BRIDGE_CODEGEN` is defined; otherwise they expand to nothing, so normal compilation emits no warnings.

## Testing instructions

### C++ smoke test

```bash
# Build and run
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/Release/dcb_smoke.exe
```

Covers oneshot cross-thread wake, io not blocked while awaiting, and DartFn async e2e simulated reply.

### Dart package tests

```bash
cd dart
dart test
```

Covers sync/async/stream/DartFn, error paths, bad frames, multi-isolate sessions, and lifecycle. ~38 tests. Set `DCB_LIBRARY_PATH` if the auto-detection fails.

### Codegen demo tests

Run after building the demo native library:

```bash
cd examples/codegen_demo
dart test
```

Covers generated `BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL` bindings.

### Adding tests

- C++: add to `examples/phase1_demo/smoke_main.cpp` or create a new test runner linked against `src/runtime` sources.
- Dart: add `*_test.dart` under `dart/test/` or `examples/codegen_demo/test/`.

## Security considerations

- **No untrusted inputs**: this is a bridge between Dart and bundled C++ code. The wire protocol is not designed to parse untrusted data from the network.
- **Buffer handling**: codec validates `magic`, `version`, and payload length against buffer size. A truncated or malformed frame throws a `StateError` / `std::runtime_error` and is encoded as an error frame.
- **Native memory**: FFI allocates with `malloc` and exposes `dcb_free` for caller cleanup. Dart bindings free native output/error pointers after copying.
- **DartFn closures**: closures are held in a per-session map keyed by generated `fn_id`. They are unregistered after each reverse call. Do not pass closures that capture sensitive data unless you trust the C++ side.
- **No sandboxing**: C++ code runs natively with the host process privileges. Treat C++ business code as part of the application trust boundary.
- **Dependency integrity**: codegen toolchain is pinned by URL + SHA256 in `codegen/versions.lock` and validated on every bootstrap. Do not bypass the hash verification.

## Common pitfalls

- **Sync DartFn on the io thread**: `DartFnStringToString::callSync` and `callDartHelloSync` block the calling thread. If called on the `io_context` thread, the whole scheduler stalls. The library does not auto-offload.
- **Runtime single-threaded by design**: `asio::io_context` runs on one thread. This is intentional to reduce locking; misuse by blocking the io thread is the caller's problem.
- **Generated code is not a build step**: codegen must be run manually after API header changes. The Native Assets hook (Phase 3) will only compile and link, not regenerate code.
- **No cancellation**: there is no general async cancellation. Stream subscription cancellation only stops new events from being delivered; C++ side continues running and silently drops late `add()` calls.
- **No ABI/API stability**: version is `0.1.0-dev.2`. Method ids may change, wire format may change, generated code may change.

## Where to find more

| Doc | Content |
|-----|---------|
| `README.md` | English project overview, quick start, status. |
| `README.zh-CN.md` | Chinese overview. |
| `docs/frb_and_cpp_bridge_design.md` | Full design, FRB comparison, codegen model (Chinese). |
| `docs/progress.md` | Landed checklist, current phase, next steps. |
| `docs/known_issues.md` | Resolved issues (DartFn oneshot, etc.) and accepted trade-offs. |
| `codegen/README.md` | Codegen toolchain, `dart_cpp_bridge.yaml`, generated layers. |
| `examples/codegen_demo/README.md` | Phase 2 fixture end-to-end instructions. |
| `dart/README.md` | Dart package status and minimal usage. |
| `dart/CHANGELOG.md` | Pub package changelog. |
