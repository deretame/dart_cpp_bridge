# Changelog

## 1.2.4

- Version aligned with `dart_cpp_bridge` 1.2.4 (async_simple uthread build
  fix; no tooling changes).

## 1.2.3

- Version aligned with `dart_cpp_bridge` 1.2.3 (macOS per-architecture build
  fix; no tooling changes).

## 1.2.2

- Fixed: `dcb_gen_tool init` now generates `native/cmake/dcb_bridge.cmake`
  with platform-aware `file://` rootUri conversion (Windows:
  `file:///C:/...` → `C:/...`; other platforms: `file:///home/...` →
  `/home/...`), matching `dcb_find_package.cmake`.
- `examples/codegen_demo` CMake updated to the same path handling.
- Version aligned with `dart_cpp_bridge` 1.2.2.

## 1.2.1

- Fixed: codegen works without the CMake-fetched async-simple headers again.
  Added a parse-only `async_simple/Signal.h` stub for the cancellable-sleep
  runtime headers; missing stubs previously made libclang fail to parse the
  scanned API headers and could silently degrade template types
  (`std::vector`, `std::unordered_map`, ...) to `int` in generated bindings.
- Version aligned with `dart_cpp_bridge` 1.2.1.

## 1.2.0

- Version aligned with `dart_cpp_bridge` 1.2.0.
- Verified against the new cancellable-sleep / Signal-Slot / collect APIs:
  regenerated demo bindings cover `cancellableTask`, `collectAll` /
  `collectAny`, and the foreign-runtime native timer tests.
- No breaking changes: CLI behavior and generated output are unchanged.

## 1.1.1

- `dcb_gen_tool init`: extract dart_cpp_bridge lookup and generated-wire check
  into `native/cmake/dcb_bridge.cmake`, so the root `native/CMakeLists.txt`
  stays focused on the library target.
- `dcb_gen_tool generate`: when no config path is given, default to
  `dart_cpp_bridge.yaml` in the current directory.

## 1.1.0

- Stable release aligned with `dart_cpp_bridge` 1.1.0.
- `dcb_gen_tool init --name <native_lib_name>`: `--name` now controls the native
  library / CMake target name and may differ from the Dart package name.
- `dcb_gen_tool init`: skip logic is now per-file, so existing empty `native/api/`
  or `native/api_impl/` directories still get starter files.
- `dcb_gen_tool`: tool version is now read from `pubspec.yaml` instead of a
  hard-coded constant.
- Updated CLI help text and usage examples.

## 1.0.0

- Stable release aligned with `dart_cpp_bridge` 1.0.0.
- `dcb_gen_tool generate` — full codegen pipeline for sync / async / normal / stream / DartFn APIs.
- `dcb_gen_tool init` — scaffold `native/CMakeLists.txt`, `hook/build.dart`, and `dart_cpp_bridge.yaml`.
- Version consistency check before codegen.
- Pinned Python 3.13 + libclang-ng 22.1 toolchain with SHA-256 verification.
- Supports Windows (x86_64, aarch64), Linux (x86_64, aarch64), macOS (x86_64, aarch64).

## 0.1.0-dev.2

- `dcb_gen_tool init`: auto-read `pubspec.yaml` name for `dart_package` (no `--name`
  needed in existing projects).
- `dcb_gen_tool init`: generate `native/CMakeLists.txt` (instead of root) with
  compact bootstrap using `dcb_find_package.cmake` shared module.
- `dcb_gen_tool init`: generate `hook/build.dart` template with `sourceDir: 'native'`.
- `dcb_gen_tool init`: relaxed conflict detection — skip existing files instead of
  aborting (only `dart_cpp_bridge.yaml` is a hard block).
- `parse_api.py`: deduplicate include paths (avoid adding dart_cpp_bridge
  native/include when already present via relative path).

## 0.1.0

- Initial release.
- `dcb_gen_tool generate` — full codegen pipeline (C++ parse → Dart/C++ generation).
- `dcb_gen_tool bootstrap` — download pinned Python 3.13 + libclang-ng 22.1 toolchain
  with SHA-256 verification.
- `dcb_gen_tool doctor` — environment health check.
- Supports Windows (x86_64, aarch64), Linux (x86_64, aarch64),
  macOS (x86_64, aarch64).
- Global activation: `dart pub global activate dcb_gen_tool`.
