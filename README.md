# [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge)

**English** | [中文](README.zh-CN.md)

[![pub package](https://img.shields.io/pub/v/dart_cpp_bridge.svg)](https://pub.dev/packages/dart_cpp_bridge)
[![GitHub stars](https://img.shields.io/github/stars/deretame/dart_cpp_bridge?logo=github&style=flat)](https://github.com/deretame/dart_cpp_bridge)

Dart/Flutter ↔ C++20 binding generator, inspired by [flutter_rust_bridge](https://cjycode.com/flutter_rust_bridge/).

Write normal C++20 code and call it from Dart/Flutter, with sync, async, stream, and Dart-closure reverse calls.

## What's this?

- Write **plain C++20** functions and classes — no hand-written FFI boilerplate
- Codegen generates Dart API + C++ wire dispatch from annotated headers
- Built-in runtime based on Asio + stdexec senders and coroutines
- Supports Android, iOS, Windows, Linux, and macOS

## Documentation versions

- **v1** — released 1.x line (current published version: 1.3.0), based on
  async-simple. Use this when maintaining an existing v1 application.
- **v2.0.0** — the current major line in this repository, based on stdexec.
  It contains a broad C++ async/concurrency migration; read the migration
  notice below before upgrading from v1.

See the [version guide](docs/versioning.md) before copying C++ async examples.

## v2.0.0 migration notice

v2 is a major release with a broad migration from async-simple to stdexec.
The Dart API, C ABI, wire protocol, generated file layout, and method-ID
contracts remain stable, but C++ async business code is not source-compatible
with v1.

Before upgrading from v1:

- Return `stdexec::task<T>` or another stdexec sender from async C++ APIs;
  `async_simple::coro::Lazy<T>` is no longer the v2 model.
- Replace `Executor` / `.via(...)` with stdexec schedulers and current
  `starts_on`, `on`, `continues_on`, and `sync_wait` usage. Replace
  `Signal` / `Slot` cancellation with stop tokens.
- Regenerate bindings with the matching `dcb_gen_tool` 2.0.0 release. Do not
  mix v1 generated native code, runtime headers, and v2 tooling.
- Update native CMake integration for C++20 and the vendored stdexec target
  (`STDEXEC::stdexec`), and keep blocking work off the single-threaded I/O
  scheduler.

## Quickstart

See the documentation for a complete quickstart:

- English: <https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文: <https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## Show me the code

**C++**

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <stdexec/execution.hpp>
#include <string>

BRIDGE_SYNC int32_t add(int32_t a, int32_t b) { return a + b; }

BRIDGE_ASYNC
stdexec::task<std::string> greet(std::string name) {
  co_return "Hello, " + name;
}
```

**Dart**

```dart
import 'package:my_app/src/native_gen/api/api_fn.dart';

void main() async {
  await DcbLib.init();

  print(add(a: 1, b: 2));            // 3
  print(await greet(name: 'World')); // Hello, World
}
```

## Documentation

- Homepage: <https://deretame.github.io/dart_cpp_bridge/>
- Quickstart: <https://deretame.github.io/dart_cpp_bridge/getting-started/>
- GitHub: <https://github.com/deretame/dart_cpp_bridge>

## Acknowledgments

- [Flutter Rust Bridge](https://github.com/fzyzcjy/flutter_rust_bridge) — architecture and product shape inspiration
- [Asio](https://think-async.com/Asio/) — event loop and asynchronous I/O
- [stdexec](https://github.com/NVIDIA/stdexec) — sender/receiver and scheduler foundation for v2
- [concurrentqueue](https://github.com/cameron314/concurrentqueue) — lock-free concurrent queue
- Dart / Flutter team — FFI, Isolate, NativeFinalizer, and the broader Dart native ecosystem

## License

MIT — see [LICENSE](LICENSE).
