# [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge)

[English](README.md) | **中文**

[![pub package](https://img.shields.io/pub/v/dart_cpp_bridge.svg)](https://pub.dev/packages/dart_cpp_bridge)
[![GitHub stars](https://img.shields.io/github/stars/deretame/dart_cpp_bridge?logo=github&style=flat)](https://github.com/deretame/dart_cpp_bridge)

Dart/Flutter ↔ C++20 绑定生成器，灵感来自 [flutter_rust_bridge](https://cjycode.com/flutter_rust_bridge/)。

用普通的 C++20 代码就能被 Dart/Flutter 调用，支持同步、异步、流式以及 Dart 闭包反向调用。

## 这是什么？

- 写**普通 C++20** 函数和类，无需手写 FFI 胶水
- 代码生成器从带注解的头文件生成 Dart API + C++ wire dispatch
- 内置基于 Asio + stdexec sender / 协程的运行时
- 支持 Android、iOS、Windows、Linux、macOS

## 文档版本

- **v1** — 已发布的 1.x，当前发布版本为 1.3.0，基于 async-simple；维护
  现有 v1 项目时请使用这套文档。
- **v2.0.0** — 当前仓库的主版本线，基于 stdexec。它包含大范围的 C++
  异步/并发模型迁移，从 v1 升级前请先阅读下面的迁移提示。

复制 C++ 异步示例前，请先阅读[版本说明](docs/versioning.zh-CN.md)。

## v2.0.0 迁移提示

v2 是一次从 async-simple 迁移到 stdexec 的大版本更新。Dart API、C ABI、
wire protocol、生成文件布局和 method ID 契约保持稳定，但 C++ 异步业务代码
与 v1 不保持源码兼容。

从 v1 升级时请注意：

- C++ 异步 API 应返回 `stdexec::task<T>` 或其他 stdexec sender；v2 不再以
  `async_simple::coro::Lazy<T>` 作为异步模型。
- 使用 stdexec scheduler 以及当前的 `starts_on`、`on`、`continues_on`、
  `sync_wait`；取消机制从 `Signal` / `Slot` 改为 stop token。
- 使用匹配的 `dcb_gen_tool` 2.0.0 重新生成绑定，不要混用 v1 生成的 native
  代码、运行时头文件和 v2 工具链。
- native CMake 集成需要 C++20 和 vendored stdexec target
  （`STDEXEC::stdexec`）；阻塞工作不要放在单线程 I/O scheduler 上执行。

## 快速开始

完整快速开始见文档：

- 英文：<https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文：<https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## 看看代码

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

## 文档

- 首页：<https://deretame.github.io/dart_cpp_bridge/>
- 快速开始：<https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>
- GitHub：<https://github.com/deretame/dart_cpp_bridge>

## 致谢

- [Flutter Rust Bridge](https://github.com/fzyzcjy/flutter_rust_bridge) — 架构与产品形态参考
- [Asio](https://think-async.com/Asio/) — 事件循环与异步 I/O
- [stdexec](https://github.com/NVIDIA/stdexec) — v2 的 sender/receiver 与 scheduler 基础设施
- [concurrentqueue](https://github.com/cameron314/concurrentqueue) — 无锁并发队列
- Dart / Flutter 团队 — FFI、Isolate、NativeFinalizer 等 Dart 原生能力

## 许可

MIT — 见 [LICENSE](LICENSE)。
