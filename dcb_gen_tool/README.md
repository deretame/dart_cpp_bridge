# dcb_gen_tool

[![pub package](https://img.shields.io/pub/v/dcb_gen_tool.svg)](https://pub.dev/packages/dcb_gen_tool)

Code generation CLI for [dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge).

Parses annotated C++ headers and generates the Dart/C++ bridge code:
sync, async, stream, and DartFn reverse calls, plus data classes and opaque classes.

## What it does

- Scans C++ headers for `BRIDGE_SYNC`, `BRIDGE_ASYNC`, `BRIDGE_NORMAL`, `BRIDGE_DATA_CLASS`, `BRIDGE_OPAQUE`, and other markers
- Generates the Dart API (`lib/src/native_gen/`) and C++ wire dispatch (`native/generated/`)
- Downloads a pinned, hash-verified Python + libclang toolchain on first run — no host Python or LLVM needed

## v2.0.0 migration notice

Version 2.0.0 targets the stdexec-based runtime and includes a broad migration
from the v1 async-simple model. The generated Dart API, wire format, file layout,
and method-ID contracts remain stable, but generated C++ async code is not
source-compatible with v1.

When upgrading a project:

- Keep `dcb_gen_tool` and `dart_cpp_bridge` on the same `2.0.0` version line.
- Change async C++ declarations to return `stdexec::task<T>` or another
  stdexec sender, then regenerate all bindings.
- Do not mix v1 generated files or async-simple headers with v2 runtime headers.
- Generated async dispatch uses a zero-capture coroutine IIFE; pass state as
  parameters rather than capturing local variables in lazy coroutines.
- Run generation through the Dart CLI (`dart run bin/dcb_gen_tool.dart generate`)
  so formatting and package-root handling are applied consistently.

## Quickstart

See the documentation for installation and usage:

- English: <https://deretame.github.io/dart_cpp_bridge/getting-started/>
- 中文: <https://deretame.github.io/dart_cpp_bridge/zh-cn/getting-started/>

## License

MIT — see [LICENSE](../LICENSE).
