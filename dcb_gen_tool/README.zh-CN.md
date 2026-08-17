# dcb_gen_tool

[![pub package](https://img.shields.io/pub/v/dcb_gen_tool.svg)](https://pub.dev/packages/dcb_gen_tool)

[dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge) 的代码生成 CLI 工具。

解析带注解的 C++ 头文件，自动生成 Dart/C++ 桥接代码：
同步、异步、流、DartFn 反向调用，以及数据类和不透明类。

## 功能

- 扫描 C++ 头文件中的 `BRIDGE_SYNC`、`BRIDGE_ASYNC`、`BRIDGE_NORMAL`、`BRIDGE_DATA_CLASS`、`BRIDGE_OPAQUE` 等标记
- 生成 Dart API（`lib/src/native_gen/`）与 C++ wire dispatch（`native/generated/`）
- 首次运行自动下载经 SHA-256 校验的固定版本 Python + libclang 工具链，无需本地安装 Python 或 LLVM

## v2.0.0 迁移提示

2.0.0 面向基于 stdexec 的运行时，包含从 v1 async-simple 模型迁移而来的大范围
改动。生成的 Dart API、wire format、文件布局和 method ID 契约保持稳定，但生成的
C++ 异步代码与 v1 不保持源码兼容。

升级项目时请注意：

- `dcb_gen_tool` 与 `dart_cpp_bridge` 必须使用同一条 `2.0.0` 版本线。
- C++ 异步声明应返回 `stdexec::task<T>` 或其他 stdexec sender，然后重新生成全部绑定。
- 不要把 v1 生成文件或 async-simple 头文件与 v2 运行时头文件混用。
- 生成的异步 dispatch 使用零捕获 coroutine IIFE；lazy coroutine 中的状态应通过参数传入，
  不要捕获局部变量。
- 请通过 Dart CLI 运行生成（`dart run bin/dcb_gen_tool.dart generate`），以确保格式化和
  package-root 处理逻辑一致。

## 快速开始

安装与使用详情请参考文档：

- English: <https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文: <https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## 许可证

MIT —— 详见 [LICENSE](../LICENSE)。
