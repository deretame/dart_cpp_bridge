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
