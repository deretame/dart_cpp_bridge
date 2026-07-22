# dart_cpp_bridge

[![pub package](https://img.shields.io/pub/v/dart_cpp_bridge.svg)](https://pub.dev/packages/dart_cpp_bridge)

**Dart-facing package** for an experimental **Dart ↔ C++20** bridge (FRB-like).

> **Dev / early preview** (`0.1.0-dev.2`). Native library is **not** built by this package yet (no build hooks). You must compile the C++ dynamic library yourself from the monorepo.

See also [`example/example.dart`](example/example.dart).

## Full documentation

This package is developed inside a monorepo. **Do not maintain a second long README here** — use the repo docs:

| | |
|--|--|
| English | [README.md](https://github.com/deretame/dart_cpp_bridge/blob/main/README.md) |
| 中文 | [README.zh-CN.md](https://github.com/deretame/dart_cpp_bridge/blob/main/README.zh-CN.md) |
| Design / progress | [docs/](https://github.com/deretame/dart_cpp_bridge/tree/main/docs) |
| Issues | [github.com/deretame/dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge/issues) |

## What this package is

- Dart FFI bindings + session / Completer / Stream / DartFn plumbing
- Companion to the C++ runtime in the same repo (`include/`, `src/`)
- Requires **Dart ≥ 3.10**

## Minimal usage

```dart
import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

Future<void> main() async {
  final b = await DartCppBridge.init(
    libraryPath: 'path/to/dart_cpp_bridge.dll', // .so / .dylib on other OS
  );
  print(b.bridgeVersion());
  print(await b.add(40, 2));
  b.shutdown(); // main isolate only, on process exit
}
```

Build the native library from the repo root (CMake + C++20); see the main README.

## License

MIT — see [LICENSE](LICENSE).
