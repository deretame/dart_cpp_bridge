# [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge)

[![pub package](https://img.shields.io/pub/v/dart_cpp_bridge.svg)](https://pub.dev/packages/dart_cpp_bridge)

Dart/Flutter ↔ C++20 binding generator, inspired by [flutter_rust_bridge](https://cjycode.com/flutter_rust_bridge/).

Write normal C++ code and call it from Dart/Flutter, with sync, async, stream, and Dart-closure reverse calls.

## What's this?

- Write **plain C++20** functions and classes — no hand-written FFI boilerplate
- Codegen generates Dart API + C++ wire dispatch from annotated headers
- Built-in runtime based on Asio + async-simple coroutines
- Supports Android, iOS, Windows, Linux, and macOS

## Quickstart

See the documentation for a complete quickstart:

- English: <https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文: <https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## Show me the code

**C++**

```cpp
#include <dart_cpp_bridge/annotate.h>

BRIDGE_SYNC int32_t add(int32_t a, int32_t b) { return a + b; }

BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> greet(std::string name) {
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
- [async-simple](https://github.com/alibaba/async-simple) — C++20 coroutine runtime
- [concurrentqueue](https://github.com/cameron314/concurrentqueue) — lock-free concurrent queue
- Dart / Flutter team — FFI, Isolate, NativeFinalizer, and the broader Dart native ecosystem

## License

MIT — see [LICENSE](LICENSE).
