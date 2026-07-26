# Changelog

## 0.1.0

- Initial release.
- `dcb_gen generate` — full codegen pipeline (C++ parse → Dart/C++ generation).
- `dcb_gen bootstrap` — download pinned Python 3.13 + libclang-ng 22.1 toolchain
  with SHA-256 verification.
- `dcb_gen doctor` — environment health check.
- Supports Windows (x86_64, aarch64), Linux (x86_64, aarch64),
  macOS (x86_64, aarch64).
- Global activation: `dart pub global activate dcb_gen_tool`.
