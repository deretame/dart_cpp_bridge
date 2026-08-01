# [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge)

[English](README.md) | **中文**

[![pub package](https://img.shields.io/pub/v/dart_cpp_bridge.svg)](https://pub.dev/packages/dart_cpp_bridge)
[![GitHub stars](https://img.shields.io/github/stars/deretame/dart_cpp_bridge?logo=github&style=flat)](https://github.com/deretame/dart_cpp_bridge)

Dart/Flutter ↔ C++20 绑定生成器，灵感来自 [flutter_rust_bridge](https://cjycode.com/flutter_rust_bridge/)。

用普通的 C++20 代码就能被 Dart/Flutter 调用，支持同步、异步、流式以及 Dart 闭包反向调用。

## 这是什么？

- 写**普通 C++20** 函数和类，无需手写 FFI 胶水
- 代码生成器从带注解的头文件生成 Dart API + C++ wire dispatch
- 内置基于 Asio + async-simple 协程的运行时
- 支持 Android、iOS、Windows、Linux、macOS

## 快速开始

完整快速开始见文档：

- 英文：<https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文：<https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## 看看代码

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

## 文档

- 首页：<https://deretame.github.io/dart_cpp_bridge/>
- 快速开始：<https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>
- GitHub：<https://github.com/deretame/dart_cpp_bridge>

## 致谢

- [Flutter Rust Bridge](https://github.com/fzyzcjy/flutter_rust_bridge) — 架构与产品形态参考
- [Asio](https://think-async.com/Asio/) — 事件循环与异步 I/O
- [async-simple](https://github.com/alibaba/async_simple) — C++20 协程运行时
- [concurrentqueue](https://github.com/cameron314/concurrentqueue) — 无锁并发队列
- Dart / Flutter 团队 — FFI、Isolate、NativeFinalizer 等 Dart 原生能力

## 许可

MIT — 见 [LICENSE](LICENSE)。
