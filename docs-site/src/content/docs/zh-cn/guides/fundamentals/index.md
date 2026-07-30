---
title: 概述
description: dart_cpp_bridge 项目介绍与能力概览
---

`dart_cpp_bridge` 是一个 **Dart ↔ C++20** 互操作桥接库，灵感来自 [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/)。它让现有的 C/C++ 代码以接近 Dart `async` / `await` / `Stream` 的体验暴露给 Dart/Flutter，而业务 C++ 代码只需写普通函数或 `async_simple::coro::Lazy<T>` 协程。

## 能做什么

- **同步调用**：`BRIDGE_SYNC` 函数直接返回，Dart 侧为同步调用
- **异步协程**：`BRIDGE_ASYNC` 函数返回 `Lazy<T>`，在 bridge 的 io 线程上 `co_await` 挂起，不阻塞线程
- **Stream 流**：C++ 通过 `StreamSink` 向 Dart 持续推送数据
- **DartFn 反向调用**：C++ 调用 Dart 闭包（`Future` 风格），支持参数式和持久化回调
- **C++ 异常透传**：C++ 抛出的异常在 wire 边界被捕获并编码为 Dart 异常，不会导致进程崩溃
- **代码生成**：解析 C++ 头文件中的 `BRIDGE_*` 标记，自动生成 Dart FFI 绑定、wire dispatch 和序列化代码
- **跨平台**：Windows、Linux、macOS、iOS、Android 五端支持
- **纯 C API**：`cbridge.h` + `dcb_codec.h` 提供 C99 兼容的 callback 风格入口，供纯 C 项目或其他语言 runtime 使用
- **外部运行时集成**：libuv、glib、自定义事件循环可通过 `ForeignExecutor` 接入 bridge 的协程系统
- **协程通道**：`co::oneshot` / `co::mpsc` 构建跨线程 / 跨运行时的非阻塞流水线

## 设计理念

核心原则：

> **业务 C++ 代码以普通函数或 `async_simple::coro::Lazy<T>` 编写；桥接层处理编解码、调度和 Dart API 生成。**

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

- **Runtime**：进程级单例，包含 `asio::io_context` 事件循环、`AsioExecutor` 和阻塞线程池
- **Session**：每个调用 `DartCppBridge.init()` 的 Isolate 对应一个 Session，管理 reply port 和 DartFn 闭包注册表
- **Wire**：小端二进制帧，C++ 异常在 wire 边界被捕获并编码为错误帧，不会跨越 FFI

## 内置基础运行时

bridge 自带一个基于 **asio + async-simple** 的运行时，业务代码通常不需要自己创建事件循环或 executor。可直接使用：

- `dcb::spawn` / `spawn_detached` / `spawn_blocking` — 启动协程和卸载阻塞任务
- `co::oneshot` / `co::mpsc` — 协程通道
- `async_simple::coro::sleep` — 非阻塞定时（底层 `asio::steady_timer`）

详见 [基础运行时](/dart_cpp_bridge/guides/fundamentals/runtime/) 和 [async-simple 协程入门](/dart_cpp_bridge/guides/fundamentals/async-simple/)。

## 技术栈

| 层 | 技术 | 说明 |
|---|---|---|
| C++ 标准 | C++20 | 协程、concepts；需要较新的 MSVC / GCC / Clang |
| 事件循环 | Asio standalone | 单线程 `io_context` |
| 协程与通道 | async-simple | `Lazy`、`Executor`；bridge 在其上提供 `co::oneshot` / `co::mpsc` |
| 队列 | moodycamel::ConcurrentQueue | `co::mpsc` 底层无锁队列 |
| Dart 侧 | Dart 3 + `package:ffi` | Isolates、`ReceivePort`、`Completer` / `Stream` |
| 代码生成 | Python 3.13 + libclang-ng | 解析 C++ 头文件生成绑定；工具链版本锁定 |
| 构建 | CMake 3.24+ | FetchContent 拉取 Asio / async-simple / ConcurrentQueue |

## 下一步

- [快速开始](/dart_cpp_bridge/getting-started/) — 创建项目、安装工具、生成第一批绑定
- [架构设计](/dart_cpp_bridge/guides/fundamentals/architecture/) — 核心组件与调用流程
- [函数标记选择指南](/dart_cpp_bridge/guides/fundamentals/markers/) — 选 `BRIDGE_SYNC` / `ASYNC` / `NORMAL` / `Stream` / `DartFn`
- [生命周期管理](/dart_cpp_bridge/guides/fundamentals/lifecycle/) — Runtime、Session、Opaque 对象、NativeFinalizer
- [异常与错误处理](/dart_cpp_bridge/guides/fundamentals/errors/) — C++ ↔ Dart 异常透传规则
- [项目目录结构](/dart_cpp_bridge/guides/fundamentals/project-structure/) — 手写文件与生成产物
- [C++ ↔ Dart 类型翻译](/dart_cpp_bridge/guides/fundamentals/encoding/) — C++ 类型如何映射到 Dart 类型
- [基础运行时](/dart_cpp_bridge/guides/fundamentals/runtime/) — Runtime、spawn、channel、sleep
- [async-simple 协程入门](/dart_cpp_bridge/guides/fundamentals/async-simple/) — `Lazy`、`Executor`、`co_await` 行为
- [类型映射](/dart_cpp_bridge/codegen/type-mapping/) — C++ ↔ Dart 类型与约束
