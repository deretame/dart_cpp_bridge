# dcb_gen_tool

[![pub package](https://img.shields.io/pub/v/dcb_gen_tool.svg)](https://pub.dev/packages/dcb_gen_tool)

Code generation CLI for [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge).

Parses annotated C++ headers and generates the Dart/C++ bridge code:
sync, async, stream, and DartFn reverse calls, plus data classes and opaque classes.

## What it does

- Scans C++ headers for `BRIDGE_SYNC`, `BRIDGE_ASYNC`, `BRIDGE_NORMAL`, `BRIDGE_DATA_CLASS`, `BRIDGE_OPAQUE`, and other markers
- Generates the Dart API (`lib/src/native_gen/`) and C++ wire dispatch (`native/generated/`)
- Downloads a pinned, hash-verified Python + libclang toolchain on first run — no host Python or LLVM needed

## Quickstart

See the documentation for installation and usage:

- English: <https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文: <https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## License

MIT — see [LICENSE](../LICENSE).
