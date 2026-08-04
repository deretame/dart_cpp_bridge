# AGENTS.md — dart_cpp_bridge

> Quick reference for AI coding agents working on `dart_cpp_bridge`.
> Read this if you know nothing about the project. For deep design context, see `docs/frb_and_cpp_bridge_design.md` (Chinese) and `README.md` (English).

## Current initiative: async-simple → stdexec migration

The project is migrating its C++ concurrency foundation from
[async-simple](https://github.com/alibaba/async_simple) (`Lazy`/`Executor`/`Signal`)
to **stdexec** (P2300 senders/receivers, the future `std::execution`).

**Authoritative references — read before writing any async C++:**

- `docs/cpp26_executor_model_usage.md` — usage guide for this repo's stdexec
  (Chinese). Core examples are compile- and run-verified against the vendored
  clone. **Follow it for any sender/scheduler/task code.**
- `third_party/stdexec` — vendored stdexec @ `f0e8ae6f` (≈ v0.11.0,
  nvhpc-26.05 baseline). Header-only, **C++20 only** (no C++26 needed).
  Integrate with `add_subdirectory(third_party/stdexec)` +
  `target_link_libraries(... PRIVATE STDEXEC::stdexec)`.

**Migration rules:**

1. **Public contracts do not change.** The C ABI (`ffi.h`, `cbridge.h`,
   `foreign_runtime.h`), wire protocol, Dart public API, and generated-code
   contracts stay stable (see [Compatibility policy](#compatibility-policy)).
   The migration changes C++ internals and the C++ business-code coroutine
   surface only.
2. New or touched async C++ code uses **stdexec senders / `stdexec::task<T>`**
   (or `exec::task<T>` when scheduler affinity is needed). Do not add new
   `async_simple::coro::Lazy` usages; legacy `Lazy` stays only until its
   module is migrated.
3. Use only **current** stdexec names — no deprecated aliases
   (`starts_on`/`continues_on`/`read_env`/`write_env`, not
   `start_on`/`transfer`/`read`/`write`). See the usage guide §1.3.
4. **Codegen coupling — change in lockstep.** "Async function" currently means
   "returns `async_simple::coro::Lazy`" to the generator. When the runtime
   switches coroutine types, update all of these in the same change:
   - `dcb_gen_tool/scripts/parse_api.py` (Lazy detection/unwrapping, ~:764, :1009)
   - `dcb_gen_tool/scripts/generate.py` (emits `Lazy<>` code + includes)
   - `dcb_gen_tool/stubs/` (parse-only stub headers for libclang)
   - `dcb_gen_tool/test/run_tests.py` fixtures and `lib/src/init.dart` template
   - The scanned-header include rule (see [Code style](#code-style-guidelines)).
5. Cancellation moves from `Signal`/`Slot` to **stop tokens**
   (`inplace_stop_source`/`stop_token`); semantics stay cooperative.

**API mapping (approximate — details in the usage guide):**

| async-simple                              | stdexec                                                                                          |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------ |
| `async_simple::coro::Lazy<T>`             | `stdexec::task<T>` (coroutine) or a sender pipeline; `exec::task<T>` for scheduler affinity      |
| `async_simple::Executor` (`AsioExecutor`, `ForeignExecutor`) | a `stdexec::scheduler` (`schedule(sch)` → sender); wrap the Asio loop with a scheduler adaptor |
| `lazy.via(ex)`                            | `stdexec::starts_on(sch, sndr)` / `stdexec::on(sch, sndr)`; in `exec::task`: `co_await exec::reschedule_coroutine_on(sch)` |
| `syncAwait(lazy)`                         | `stdexec::sync_wait(sndr)` (non-coroutine functions only; calling it on the io thread self-deadlocks — unchanged rule) |
| `collectAll(...)`                         | `stdexec::when_all(...)` (values flattened)                                                      |
| `collectAny(...)`                         | `exec::when_any(...)` (winner requests stop on losers)                                           |
| `Signal` / `Slot` (Terminate)             | `inplace_stop_source` / `stop_token` / `inplace_stop_callback` (cooperative)                     |
| `coro::sleep(dur, ex)`                    | `exec::schedule_after(timed_sched, dur)` (needs a timed scheduler)                               |
| `Future<T>` / `Promise<T>`                | oneshot channel / sender (migrate together with `channel.hpp`)                                   |

**Migration touchpoints (where async-simple lives today):**

- `dart/native/include/dart_cpp_bridge/`: `asio_executor.hpp`,
  `cbridge_wait.hpp`, `channel.hpp`, `dart_fn.hpp`, `runtime.hpp`,
  `session.hpp`, `foreign_executor.hpp`, `foreign_runtime.h`, `stream*.hpp`
- `dart/native/src/`: `cbridge.cpp`, `runtime/runtime.cpp`
- `dcb_gen_tool/` (see rule 4 above)
- `examples/*/native/` (API headers, generated `wire_dispatch.*`,
  `worker_runtime.hpp`)
- `docs-site/` async-simple guides (last phase)

## Project overview

`dart_cpp_bridge` is a released **Dart ↔ C++20** interoperability bridge inspired by [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/). It lets existing C/C++ code expose sync, async, stream, and reverse Dart-closure APIs to Dart/Flutter with a runtime that feels like FRB.

- **Status**: Released / stable. `dart_cpp_bridge` (dart package) and
  `dcb_gen_tool` are both published at **1.2.4**. Public APIs are stable;
  avoid breaking changes (see [Compatibility policy](#compatibility-policy)).
- **Goal**: Give C++ libraries a clean integration surface (sync / async / stream / DartFn reverse calls) using C++20 coroutines/senders and a single-threaded Asio event loop.
- **Repository**: <https://github.com/deretame/dart_cpp_bridge>

### High-level architecture

```text
Dart Isolate(s)
  Session per Isolate (one long-lived reply port)
  Future / Stream / DartFn callbacks
       ⇅  FFI binary frames
Runtime (process-wide)
  asio::io_context (single-threaded) + scheduler/executor adaptor
  thread pool (blocking / normal work)
  wire: sync / async coroutine-sender / stream / DartFn
```

Core principle: **business C++ code is written as normal functions or one coroutine/sender type (`async_simple::coro::Lazy<T>` today, stdexec after the migration); the bridge handles codec, scheduling, and Dart API generation.** Do not invent bridge-specific Future/Stream wrapper types in business code.

## Compatibility policy

The project is in a **released / stable state** (1.2.4). Treat every public
surface as part of the compatibility contract:

- Dart package public API (`dart/lib/`) and generated code contracts;
- C ABI exports: `ffi.h`, `cbridge.h`, `foreign_runtime.h` (including
  `dart_cpp_bridge` FFI functions and the `dcb_*` C bridge API);
- Public C++ headers under `dart/native/include/dart_cpp_bridge/`;
- The wire protocol (frame layout, `msg_type` values, `method_id` semantics);
- `dcb_gen_tool` CLI behavior, `dart_cpp_bridge.yaml` config schema, and
  generated output file names/shapes.

Rules for changes:

- **No breaking changes without a major version bump.** Do not remove, rename,
  or change existing functions, enum values, struct fields, wire frame
  fields, `method_id`s, or generated file names. Do not silently change
  behavior that callers may rely on.
- **Prefer additive extensions.** Add new functions/APIs, `_ex` / `_v2`
  variants with the old entry point kept as a wrapper, optional new callbacks
  or fields defaulting to `NULL`/absent, and new `method_id`s — never
  renumber existing ones.
- **Deprecate, don't delete.** If something must change, keep the old API
  working, document it as deprecated in the changelog, and remove it only in
  a future major version.
- Internal details (build artifacts, `examples/`, docs, private headers) are
  not API and may change freely.

The stdexec migration is **not** a license to break these contracts: keep the
C ABI, wire protocol, and Dart-side APIs stable; contain the churn to C++
internals and the documented coroutine-type switch.

## Versioning

- `dart/pubspec.yaml` and `dcb_gen_tool/pubspec.yaml` **must always carry the
  same version number**. When bumping either package, bump the other in the
  same change — even if `dcb_gen_tool` has no code changes and the update is
  only a version sync.
- Keep both changelogs in step: the package with real changes gets a normal
  entry; the other gets at least a short "version aligned with
  `dart_cpp_bridge` x.y.z" entry.

## Technology stack

| Layer        | Technology                                                   | Notes                                                                                          |
| ------------ | ------------------------------------------------------------ | ---------------------------------------------------------------------------------------------- |
| C++ standard | C++20 minimum                                                | Coroutines, concepts. Requires recent MSVC/GCC/Clang. stdexec needs **only** C++20.             |
| Event loop   | [Asio](https://think-async.com/Asio/) standalone             | `asio::io_context` single-threaded; timers, post, completion.                                   |
| Concurrency  | stdexec (P2300 senders/receivers), vendored `third_party/stdexec` | **Migrating from async-simple** (`Lazy`/`Executor`/`Signal`). See [Current initiative](#current-initiative-async-simple--stdexec-migration). |
| Dart side    | Dart 3 + `package:ffi`                                       | Isolates, `ReceivePort`, `Completer`, `Stream`, `NativeFinalizer`.                              |
| Dart SDK     | `>= 3.10.0`                                                  | `dart/pubspec.yaml` floor. Native Assets hooks need 3.10+; link hooks need 3.13+.               |
| Codegen      | Pinned Python 3.13.13 + libclang-ng 22.1.4.2                 | Downloaded from remote, cached, hash-verified. No host Python/LLVM.                             |
| Build        | CMake 3.24+                                                  | FetchContent pulls Asio/async-simple (until migration completes); stdexec is vendored. Native Assets hooks are wired (`hook/build.dart`, `hook/link.dart`). |

## Directory structure

```text
.
├── README.md / README.zh-CN.md
├── AGENTS.md                    # This file
├── third_party/stdexec/         # Vendored P2300 reference implementation (C++20, header-only)
├── docs/
│   ├── frb_and_cpp_bridge_design.md   # Design decisions (Chinese)
│   ├── cpp26_executor_model_usage.md  # stdexec usage guide (Chinese, compile-verified)
│   ├── progress.md                    # Implementation progress
│   └── known_issues.md                # Resolved/known tech debt
├── dart/                      # Dart package (pub package root) + native library
│   ├── pubspec.yaml
│   ├── native/cmake/           # CMake modules (dcb_find_package, fetch_dart_api)
│   ├── native/                # C++ native library (base runtime only)
│   │   ├── CMakeLists.txt     # Static lib + FetchContent deps (asio/async-simple)
│   │   ├── include/dart_cpp_bridge/   # Public C++ headers
│   │   │   ├── runtime.hpp    # Singleton Runtime, spawn_on_asio
│   │   │   ├── session.hpp    # Session, SessionRegistry, DartFnReply
│   │   │   ├── channel.hpp    # co::mpsc / co::oneshot coroutine channels
│   │   │   ├── dart_fn.hpp    # DartFnStringToString (sync/async reverse call)
│   │   │   ├── stream_sink.hpp # StreamSink<T>
│   │   │   ├── codec.hpp      # Wire frame + ByteReader/Writer
│   │   │   ├── ffi.h          # C ABI exported by the shared library
│   │   │   ├── asio_executor.hpp # AsioExecutor for async-simple (migration target)
│   │   │   ├── foreign_executor.hpp # ForeignExecutor for async-simple (migration target)
│   │   │   └── annotate.h     # BRIDGE_* / DCB_* codegen markers
│   │   ├── src/
│   │   │   ├── runtime/runtime.cpp  # Runtime impl, Session impl, DartFn invoke
│   │   │   ├── runtime/object_handle.cpp # Object handle registry
│   │   │   └── ffi_entry.cpp        # C ABI exports (dcb_init_dart_api, etc.)
│   │   └── third_party/dart_api/    # Dart API DL C headers (downloaded, gitignored)
│   ├── lib/                   # dart_cpp_bridge package
│   │   ├── src/bridge.dart    # DartCppBridge class
│   │   ├── src/bindings.dart  # FFI bindings
│   │   ├── src/codec.dart     # Dart codec mirror
│   │   └── dart_cpp_bridge.dart
│   ├── test/                  # FFI + codec tests
│   └── example/example.dart
├── dcb_gen_tool/              # Codegen CLI (pub global activate dcb_gen_tool)
│   ├── pubspec.yaml           # dcb_gen_tool pub package
│   ├── bin/dcb_gen_tool.dart       # CLI entry: generate / bootstrap / doctor
│   ├── lib/src/               # platform detection, lock parsing, bootstrap logic
│   ├── versions.lock          # Pinned Python + libclang-ng URLs/hashes
│   ├── scripts/               # parse/generate Python scripts (Lazy-aware; migration target)
│   ├── stubs/                 # Stub headers for codegen parsing (async_simple/ + asio/)
│   └── tests/                 # Parser defensive tests (Python)
└── examples/
    ├── base_demo/             # Hand-written wire dispatch demo + C++ smoke test
    │   ├── demo_api.cpp       # Hand-written demo wire dispatch
    │   ├── smoke_main.cpp     # C++ smoke test (no Dart VM)
    │   └── CMakeLists.txt     # Builds dart_cpp_bridge.dll + dcb_smoke
    └── codegen_demo/          # Phase 2 fixture
        ├── dart_cpp_bridge.yaml
        ├── native/api/bridge_api.h
        ├── native/api_impl/bridge_api.cpp
        ├── native/generated/  # Generated wire_dispatch.* + ir.json
        ├── lib/               # Generated Dart API + manual export
        ├── test/
        └── CMakeLists.txt
```

## Build commands

### Requirements

- CMake >= 3.24
- C++20 compiler (MSVC 2019+, GCC 10+, Clang 12+)
- Dart SDK >= 3.10.0
- Git (for FetchContent)
- Network for first C++ build (Asio/async-simple) and codegen toolchain

### C++ base library + base_demo

```bash
# 1. Fetch Dart API DL headers (one-time unless deleted)
cmake -P dart/native/cmake/fetch_dart_api.cmake

# 2. Configure base library deps (asio/async-simple)
cmake -S dart/native -B dart/native/build -DCMAKE_BUILD_TYPE=Release

# 3. Build base_demo (DLL + smoke test)
cmake -S examples/base_demo -B examples/base_demo/build -DCMAKE_BUILD_TYPE=Release
cmake --build examples/base_demo/build --config Release

# 4. Smoke test (no Dart VM)
./examples/base_demo/build/dcb_smoke                 # Linux/macOS
./examples/base_demo/build/Release/dcb_smoke.exe     # Windows
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
# 1. Configure base library deps (reuses _deps for asio/async-simple)
cmake -S dart/native -B dart/native/build -DCMAKE_BUILD_TYPE=Release

# 2. Run codegen for the demo fixture
cd examples/codegen_demo
dcb_gen_tool generate dart_cpp_bridge.yaml

# 3. Build the demo library
cmake -S . -B build
cmake --build build --config Release

# 4. Run Dart tests
dart pub get
dart test
```

## Code organization and module divisions

### C++ side

- **Public headers** (`dart/native/include/dart_cpp_bridge/`): the runtime surface. Business code and generated wire include these.
- **Runtime** (`dart/native/src/runtime/runtime.cpp`): process-wide `Runtime`, per-isolate `Session`, `SessionRegistry`, `DartFnStringToString` async implementation.
- **Wire** (`examples/base_demo/demo_api.cpp` or generated `wire_dispatch.cpp`): frame dispatch, method routing, codec, scheduling. This is the only place that knows about `request_id`, `method_id`, and port posting.
- **FFI entry** (`dart/native/src/ffi_entry.cpp`): C ABI exports used by Dart. This is the dynamic-library boundary.

### Dart side

- `dart/lib/src/bridge.dart`: high-level `DartCppBridge` class — session lifecycle, `invokeSyncMethod`, `invokeAsyncMethod`, `ticks`, `callDartHello`, etc.
- `dart/lib/src/bindings.dart`: raw FFI bindings to `dcb_*` C functions.
- `dart/lib/src/codec.dart`: frame encoding/decoding mirror of `dart/native/include/dart_cpp_bridge/codec.hpp`.

### Generated code (Phase 2 codegen)

For a user project, codegen emits three layers in Dart:

```text
api_fn.dart   # top-level functions: initBridge(), add(), ...  (preferred call site)
api.dart      # BridgeApi.instance singleton (init / dispose / forward)
api.g.dart    # BridgeApiImpl: method ids, codec, invoke* calls
```

C++ side emits `native/generated/wire_dispatch.hpp|.cpp` and `ir.json`. Business implementation stays in user-written files like `native/api_impl/bridge_api.cpp`.

## Wire protocol

Little-endian binary frame (`dart/native/include/dart_cpp_bridge/codec.hpp` and `dart/lib/src/codec.dart`):

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
- **Concurrency**: write new async business code as stdexec senders or
  `stdexec::task<T>` (`exec::task<T>` for scheduler affinity). Legacy
  `async_simple::coro::Lazy<T>` remains only in not-yet-migrated code — do
  not add new usages. Do not write custom `bridge::Future<T>` wrappers.
- **Codegen parses API headers transitively**: `native/api/*.h` and everything
  it includes are parsed by libclang; a missing or unparseable header can
  silently degrade template types (`std::vector`, `std::unordered_map`, ...) to
  `int` in generated bindings. Scanned headers may only include C++ standard
  headers, `dart_cpp_bridge/*`, and the designated coroutine header (today
  `async_simple/coro/Lazy.h`; after the migration, the stdexec task header —
  update `dcb_gen_tool/stubs` in the same change).
- **Threading**: never block the `io_context` thread. Blocking work goes to the thread pool (normal/stream kind) or a blocking-offload scheduler.
- **Error handling**: at the wire boundary always catch `const std::exception&` first, then `(...)`. Encode errors into frames; do not let exceptions propagate across FFI.
- **Dart**: follows standard Dart package conventions, `package:lints` for static analysis. Use `final` and `StateError` for runtime failures.
- **Naming**: C++ namespace `dcb`, generated wire namespace `dcb::demo`. Dart classes use `PascalCase`.
- **Codegen markers**: in user headers use `BRIDGE_SYNC`, `BRIDGE_ASYNC`, `BRIDGE_NORMAL`, `BRIDGE_EXPORT`, `BRIDGE_DATA_CLASS`, `BRIDGE_OPAQUE`, or `BRIDGE_TO_STRING` (also aliased as `DCB_*`). They expand to `__attribute__((annotate("bridge::*")))` only when `BRIDGE_CODEGEN` / `DART_CPP_BRIDGE_CODEGEN` is defined; otherwise they expand to nothing, so normal compilation emits no warnings.
  - Streams: a function is exported as Dart `Stream<T>` only when it has an export marker (`BRIDGE_SYNC`/`BRIDGE_ASYNC`/`BRIDGE_NORMAL`, typically `BRIDGE_NORMAL`) AND a required `dcb::StreamSink<T>` parameter. A `StreamSink` parameter alone does NOT export the function (the parser warns and skips it). Optional streams use `std::optional<dcb::StreamSink<T>>` on `BRIDGE_SYNC`/`BRIDGE_ASYNC`/`BRIDGE_NORMAL` functions instead (sync: events are delivered to the controller after the blocking FFI call returns).
  - `BRIDGE_DATA_CLASS`: marks a pure data class (fields only, no exported methods). Validated: no inheritance, no virtuals, no `BRIDGE_SYNC/ASYNC/NORMAL` methods.
  - `BRIDGE_OPAQUE`: marks an opaque class (methods only, public fields ignored). Aligns with FRB `RustAutoOpaque`. For field access, hand-write `BRIDGE_SYNC` getter/setter methods.
  - `BRIDGE_TO_STRING`: marks an opaque-class method as the source of the Dart `toString()` override. Must be a sync instance method returning `std::string` with no args (validated); generates a wire-call `toString()` on the wrapper.
  - `BRIDGE_PERSIST`: marks a function with DartFn params as a persistent callback registration. Dart side will NOT auto-unregister the closure after the call returns, allowing C++ to store and invoke it later. Typically combined with `BRIDGE_SYNC` (register) + a separate `BRIDGE_NORMAL` function (invoke).

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

- C++: add to `examples/base_demo/smoke_main.cpp` or create a new test runner linked against `dart/native/src/runtime` sources.
- Dart: add `*_test.dart` under `dart/test/` or `examples/codegen_demo/test/`.

## Security considerations

- **No untrusted inputs**: this is a bridge between Dart and bundled C++ code. The wire protocol is not designed to parse untrusted data from the network.
- **Buffer handling**: codec validates `magic`, `version`, and payload length against buffer size. A truncated or malformed frame throws a `StateError` / `std::runtime_error` and is encoded as an error frame.
- **Native memory**: FFI allocates with `malloc` and exposes `dcb_free` for caller cleanup. Dart bindings free native output/error pointers after copying.
- **DartFn closures**: closures are held in a per-session map keyed by generated `fn_id`. They are unregistered after each reverse call. Do not pass closures that capture sensitive data unless you trust the C++ side.
- **No sandboxing**: C++ code runs natively with the host process privileges. Treat C++ business code as part of the application trust boundary.
- **Dependency integrity**: codegen toolchain is pinned by URL + SHA256 in `dcb_gen_tool/versions.lock` and validated on every bootstrap. Do not bypass the hash verification.

## Common pitfalls

- **Sync DartFn on the io thread**: `DartFn::operator()` returns `Lazy<Ret>` (async only). For blocking contexts, use `syncAwait(dcb::spawn(fn(args...)))`. Calling `syncAwait` on the `io_context` thread is a self-deadlock. The library does not auto-offload. (Post-migration: same rule with `stdexec::sync_wait`.)
- **Runtime single-threaded by design**: `asio::io_context` runs on one thread. This is intentional to reduce locking; misuse by blocking the io thread is the caller's problem.
- **Generated code is not a build step**: codegen must be run manually after API header changes. Native Assets hooks compile and link only; they do not regenerate code.
- **No direct Dart-side cancellation**: a Dart `Future` cannot be force-cancelled. Cancellation is cooperative — today via async-simple `Signal`/`Slot` (cancellable `sleep()`, `collectAny`/`collectAll<Terminate>`), after the migration via stdexec stop tokens — and must be exposed by business code (e.g. a task_id → stop-source map plus a `cancelTask`-style API). Stream subscription cancellation only stops new events from being delivered; the C++ side continues running and silently drops late `add()` calls.
- **stdexec top pitfalls** (details in `docs/cpp26_executor_model_usage.md`):
  `co_await` binds tighter than `|` — parenthesize pipelines
  (`co_await (a | then(f))`); `co_await` of a single-value sender yields the
  bare value, not a tuple; write env with `write_env` (not `write`/`read`);
  `on()` returns to the *start* scheduler, it does not stay on the target;
  mark trailing lambdas `noexcept` before `stdexec::spawn`/`start_detached`;
  scopes must be drained (`on_empty()`/`join()`) before destruction.
- **Stable API — no breaking changes**: packages are released (`1.2.4`). Do not change `method_id`s, wire format, public signatures, or generated-code contracts without a major version bump. Prefer additive extensions such as `_ex` variants and optional callbacks (see [Compatibility policy](#compatibility-policy)).

## Where to find more

| Doc                                 | Content                                                         |
| ----------------------------------- | --------------------------------------------------------------- |
| `README.md`                         | English project overview, quick start, status.                  |
| `README.zh-CN.md`                   | Chinese overview.                                               |
| `docs/frb_and_cpp_bridge_design.md` | Full design, FRB comparison, codegen model (Chinese).           |
| `docs/cpp26_executor_model_usage.md`| stdexec / P2300 usage guide, migration reference (Chinese, compile-verified). |
| `docs/progress.md`                  | Landed checklist, current phase, next steps.                    |
| `docs/known_issues.md`              | Resolved issues (DartFn oneshot, etc.) and accepted trade-offs. |
| `dcb_gen_tool/README.md`            | Codegen toolchain, `dart_cpp_bridge.yaml`, generated layers.    |
| `examples/codegen_demo/README.md`   | Phase 2 fixture end-to-end instructions.                        |
| `dart/README.md`                    | Dart package status and minimal usage.                          |
| `dart/CHANGELOG.md`                 | Pub package changelog.                                          |
