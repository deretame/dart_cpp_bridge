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
