---
title: 概述
description: dart_cpp_bridge 项目介绍
---

`dart_cpp_bridge` 是一个实验性的 **Dart ↔ C++20** 互操作桥接库，灵感来自 [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/)。

## 设计理念

核心原则：**业务 C++ 代码以普通函数或 `async_simple::coro::Lazy<T>` 编写；桥接层处理编解码、调度和 Dart API 生成。**

不需要在业务代码中发明桥接专用的 Future/Stream 包装类型。

## 架构概览

```text
Dart Isolate(s)
  Session per Isolate (one long-lived reply port)
  Future / Stream / DartFn callbacks
       ⇅  FFI binary frames
Runtime (process-wide)
  asio::io_context (single-threaded) + AsioExecutor
  asio::thread_pool (blocking / normal work)
  wire: sync / async Lazy / stream / DartFn
```

## 技术栈

| 层 | 技术 | 说明 |
|---|---|---|
| C++ 标准 | C++20 | 协程、concepts |
| 事件循环 | Asio standalone | 单线程 `io_context` |
| 协程 | async-simple | `Lazy`, `Executor` |
| Dart | Dart 3 + `package:ffi` | Isolates, ReceivePort |
| 代码生成 | Python + libclang | 解析 C++ 头文件生成绑定 |
| 构建 | CMake 3.24+ | FetchContent 拉取依赖 |

## 状态

- **版本**: 0.1.0-dev
- **阶段**: Phase 1 + early Phase 2
- **稳定性**: 实验性，API 可能变化
