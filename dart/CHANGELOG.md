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
