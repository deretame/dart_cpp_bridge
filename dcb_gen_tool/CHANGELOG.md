# Changelog

## 0.1.0-dev.2

- `dcb_gen init`: auto-read `pubspec.yaml` name for `dart_package` (no `--name`
  needed in existing projects).
- `dcb_gen init`: generate `native/CMakeLists.txt` (instead of root) with
  compact bootstrap using `dcb_find_package.cmake` shared module.
- `dcb_gen init`: generate `hook/build.dart` template with `sourceDir: 'native'`.
- `dcb_gen init`: relaxed conflict detection — skip existing files instead of
  aborting (only `dart_cpp_bridge.yaml` is a hard block).
- `parse_api.py`: deduplicate include paths (avoid adding dart_cpp_bridge
  native/include when already present via relative path).

## 0.1.0

- Initial release.
- `dcb_gen generate` — full codegen pipeline (C++ parse → Dart/C++ generation).
- `dcb_gen bootstrap` — download pinned Python 3.13 + libclang-ng 22.1 toolchain
  with SHA-256 verification.
- `dcb_gen doctor` — environment health check.
- Supports Windows (x86_64, aarch64), Linux (x86_64, aarch64),
  macOS (x86_64, aarch64).
- Global activation: `dart pub global activate dcb_gen_tool`.
