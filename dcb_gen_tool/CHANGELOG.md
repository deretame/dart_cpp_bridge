# Changelog

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
