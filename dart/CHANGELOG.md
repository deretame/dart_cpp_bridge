## 2.2.0

- Added configurable io scheduler runner count through `DartCppBridge.init`
  and Native Assets-generated bindings; the default remains one runner.
- Added a runtime deadlock guard and documented the multi-runner boundary for
  raw `stdexec::sync_wait` calls.

## 2.1.1

- Hardened runtime lifecycle, C bridge completion, Native Assets target
  selection, and stdexec fixture integration.
- Added fail-fast codegen validation for Dart names and method-ID collisions.

## 2.1.0

- Fixed opaque-object finalization on 32-bit targets by using an ABI-safe
  native token that preserves the complete 64-bit handle.
- Opaque-object dispatch now validates that handles belong to the current
  isolate session.
- Async invocation failures now complete pending Dart `Future`s with an error
  instead of leaving them pending forever.
- Persistent DartFn registrations replace the previous closure for the same
  generated key, preventing unbounded per-session callback retention.

## 2.0.0

- **Breaking (C++ async API):** migrated the runtime and generated async
  dispatch from async-simple to stdexec senders and `stdexec::task`. The Dart
  API, C ABI, wire protocol, and generated file contracts remain compatible,
  but C++ business APIs must use the stdexec model.

- `hook.dart`: `WindowsConfig` now supports two non-MSVC compilers via
  `WindowsCompiler`:
  - `clangCl` — LLVM `clang-cl`, the MSVC-compatible clang driver. With
    `CmakeGenerator.ninja` (the default when `generator` is `null`), the
    builder initializes the MSVC environment via `vcvarsall.bat` **first**,
    then locates clang-cl: an explicit `WindowsConfig.clangClPath` wins, then
    a VS-bundled clang-cl (visible on the vcvars PATH when the "C++ Clang
    tools for Windows" component is installed), then `PATH` / LLVM installs /
    the LLVM registry key. The resolved compiler is passed via
    `-DCMAKE_C(XX)_COMPILER=<clang-cl>`. With `CmakeGenerator.msbuild`, the
    builder passes `-T clangcl`.
  - `msys2Clang` / `msys2Gcc` — GNU/MinGW-style MSYS2 ucrt64 clang / gcc
    (`x86_64-w64-windows-gnu` target). The builder resolves the MSYS2 root
    via `WindowsConfig.msys2Path` / `MSYS2_ROOT` / well-known install
    locations and passes `-DCMAKE_C(XX)_COMPILER=<msys2>\ucrt64\bin\
    clang(++).exe` (or `gcc(++).exe`) with the Ninja generator; no vcvars
    environment is needed. By default the C++ runtime is statically linked
    into the DLL (`WindowsConfig.staticRuntime`), producing a self-contained
    DLL with no MSYS2 runtime DLL dependency — no `PATH` setup needed at
    runtime. With `staticRuntime: false` the MSYS2 runtime DLLs
    (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) are
    bundled next to the output when `bundleCrt` is `true`.
- Native build: `dart/native/CMakeLists.txt` and the example CMakeLists now
  use compiler-appropriate UTF-8 flags (`/utf-8` for MSVC/clang-cl,
  `-finput-charset=UTF-8 -fexec-charset=UTF-8` for GNU-style compilers) and
  link `ws2_32`/`mswsock` on non-MSVC Windows toolchains (asio requires
  Winsock). `DcbCMakeBuilder` artifact lookup also probes the MinGW `lib`
  prefix (`lib<name>.dll`).

## 1.3.0

- New: `BRIDGE_SYNC` functions can now take `std::optional<dcb::StreamSink<T>>`
  parameters. Sync calls can emit stream events (typically by spawning a
  coroutine that sends them asynchronously); Dart receives the events from the
  reply port queue right after the blocking FFI call returns. The generated
  Dart API takes a `StreamController<T>?` input parameter and stays
  synchronous, backed by the new `DartCppBridge.invokeSyncMethodWithStream`.
- Streams: functions with a `dcb::StreamSink<T>` parameter are generated as
  Dart `Stream<T>` when they also carry an export marker (`BRIDGE_SYNC` /
  `BRIDGE_ASYNC` / `BRIDGE_NORMAL`, typically `BRIDGE_NORMAL`). The parser
  warns and skips sink-only functions without an export marker.
- `StreamSink` now holds a `std::shared_ptr<Session>`, so a long-lived sink
  stays safe even after its session is closed: late `add()` / `end()` /
  `error()` calls are silently dropped by the generation check instead of
  touching a destroyed session.
- `co::mpsc` / `co::oneshot` `recv()` awaiters now hold a `shared_ptr` to the
  channel state, so the state stays alive even if the receiver is destroyed or
  moved while a coroutine is suspended.
- codegen_demo: added a `sync_download_with_progress` fixture covering sync +
  optional `StreamSink` end to end.
- Version aligned with `dcb_gen_tool` 1.3.0.

## 1.2.4

- Fix macOS cross-architecture builds: async_simple's uthread static/shared
  targets (Darwin assembly selected by `CMAKE_SYSTEM_PROCESSOR`, which
  ignores `CMAKE_OSX_ARCHITECTURES`) are now excluded from the default CMake
  build. The runtime only uses the header-only target, so the unused uthread
  assembly is no longer compiled — previously building an x86_64 slice on an
  arm64 host (or vice versa) compiled the wrong `.S` file and failed.
- Version aligned with `dcb_gen_tool` 1.2.4 (no tooling changes).

## 1.2.3

- Fix macOS universal-binary builds: `DcbCMakeBuilder` now passes the
  hooks-provided `targetArchitecture` to CMake via
  `-DCMAKE_OSX_ARCHITECTURES` (arm64 / x86_64) instead of building the host
  architecture twice. Flutter's native-assets flow invokes the hook once per
  architecture and merges the slices with lipo; previously both invocations
  produced the same host-arch dylib and `lipo -create` failed.
- Version aligned with `dcb_gen_tool` 1.2.3 (no tooling changes).

## 1.2.2

- Version aligned with `dcb_gen_tool` 1.2.2 (codegen `init` `file://` path
  handling bugfix; no runtime API changes).

## 1.2.1

- Version aligned with `dcb_gen_tool` 1.2.1 (codegen bugfix release; no runtime
  API changes).

## 1.2.0

- Cancellable `async_simple::coro::sleep()` on `AsioExecutor`: the
  `schedule(Func, Duration, Slot*)` override now registers a
  `SignalType::Terminate` handler that cancels the underlying
  `asio::steady_timer`, so a sleep bound to a cancellation signal
  (`Lazy::setLazyLocal`, or `collectAny<Terminate>` / `collectAll<Terminate>`)
  throws `SignalException` promptly instead of waiting out the duration.
- `ForeignExecutor`: explicit `schedule(Func, Duration, Slot*)` override with
  two paths — an optional native event-loop timer, or a deactivation-safe
  waiter-thread fallback. Pending sleeps never dereference the executor after
  it is deactivated or destroyed.
- New foreign-runtime C API: `dcb_foreign_register_ex` with optional
  `dcb_schedule_after_fn` / `dcb_cancel_after_fn` native timer callbacks
  (additive; `dcb_foreign_register` is unchanged).
- Internal timer failures are logged to stderr (`[dcb] ...`) instead of being
  silently swallowed.

## 1.1.1

- Fix hot-restart safety across Dart/native boundary.
  - Encode the session id directly as the `NativeFinalizer` token pointer value,
    avoiding a cross-module `malloc`/`free` mismatch on hot restart.
  - Copy posted wire frames and send them as external typed data with a finalizer,
    so Dart owns the buffer lifetime.
  - Guard `Runtime::start()` / `Runtime::stop()` with a mutex to prevent races.
  - Catch exceptions in `dcb_session_finalizer`, object-handle drop callbacks, and
    session dispose callbacks so they never propagate across FFI.

## 1.1.0

- `DcbBuildOptions.compileCommandsPath`: configurable destination for `compile_commands.json` (relative to package root).
- `AndroidConfig.abi`: when omitted, the ABI is auto-derived from `input.config.code.targetArchitecture`.
- `WindowsConfig`: improved Visual Studio detection and automatic `vcvarsall.bat` environment initialization for Ninja builds.
- Update package description to reflect ready Native Assets build hooks.

## 1.0.0

- First stable release.
- Dart FFI session layer with sync, async, stream, and DartFn reverse-call support.
- Multi-runtime support: bridge isolated C++ runtimes via `co::oneshot` / `co::mpsc` channels.
- Foreign runtime integration (libuv demo) through `ForeignExecutor` C API.
- Pure C cross-runtime bridge API (`cbridge.h`) for non-asio / non-C++ callers.
- Native Assets hooks (`hook/link.dart`, `hook/build.dart`) for Android, iOS, Linux, macOS, and Windows.
- Vendored Dart API DL headers (no network fetch during build).
- English-only public API comments and documentation.

## 0.1.0-dev.2

- Point `repository` at monorepo package path (`.../tree/main/dart`) for pub.dev verification.
- Document public codec APIs (dartdoc coverage).
- Declare platforms: android, ios, linux, macos, windows (no web).
- Strip all demo code; base library only exposes protocol primitives and bridge infrastructure.

## 0.1.0-dev.1

- Initial **dev** publish to reserve the package name on pub.dev.
- Phase 1 hand-written Dart bindings (sync / async / stream / DartFn).
- Native library must be built separately from the monorepo (hooks not wired yet).
- See [repository README](https://github.com/deretame/dart_cpp_bridge) for full status.
