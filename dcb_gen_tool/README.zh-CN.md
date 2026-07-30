# dcb_gen_tool

[![pub package](https://img.shields.io/pub/v/dcb_gen_tool.svg)](https://pub.dev/packages/dcb_gen_tool)

[dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge) 的代码生成 CLI 工具。

解析带注解的 C++ 头文件，自动生成 Dart/C++ 桥接代码：
同步、异步、流、DartFn 反向调用，以及数据类和不透明类。

## 功能

- 扫描 C++ 头文件中的 `BRIDGE_SYNC`、`BRIDGE_ASYNC`、`BRIDGE_NORMAL`、`BRIDGE_DATA_CLASS`、`BRIDGE_OPAQUE` 等标记
- 生成 Dart API（`lib/src/native_gen/`）与 C++ wire dispatch（`native/generated/`）
- 首次运行自动下载经 SHA-256 校验的固定版本 Python + libclang 工具链，无需本地安装 Python 或 LLVM

## 快速开始

安装与使用详情请参考文档：

- English: <https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文: <https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## 许可证

MIT —— 详见 [LICENSE](../LICENSE)。
