---
title: 基础（v2）
description: 当前 stdexec 版 dart_cpp_bridge 的核心概念
---

:::caution[v2 stdexec 文档]
当前指南描述 v2 stdexec 实现。已发布的 1.x 项目请先查看
[版本文档](/dart_cpp_bridge/zh-cn/versions/)，再使用 v1 归档页面。
:::

`dart_cpp_bridge` 是一个 **Dart ↔ C++20** 桥接库，灵感来自
[Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/)。业务代码只需
写普通 C++ 函数或 `stdexec::task<T>` / sender 流水线，桥接层负责编解码、
调度、生命周期和 Dart API 生成。

## 核心能力

- **同步** — `BRIDGE_SYNC` 直接返回值；
- **异步** — `BRIDGE_ASYNC` 使用 `stdexec::task<T>` 或受支持 sender，
  Dart 侧得到 `Future<T>`；
- **Normal** — `BRIDGE_NORMAL` 把阻塞工作放到线程池；
- **Stream** — `StreamSink<T>` 生成 Dart `Stream<T>`；
- **DartFn** — C++ 可以在等待 Dart 闭包回复时挂起；
- **Channel** — `co::oneshot` 和 `co::mpsc` 连接 worker 与协程；
- **Opaque / data class** — 生成 handle 和值类型编解码；
- **跨运行时调度** — 外部事件循环提供普通 stdexec scheduler。

## 当前技术栈

| 层 | 技术 |
| --- | --- |
| C++ | C++20 |
| 异步模型 | stdexec sender / `stdexec::task` |
| 内置事件循环 | Asio `io_context`，单 io 线程 |
| 阻塞工作 | Asio thread pool |
| 取消 | stop token |
| Codegen | Python 3.13 + libclang-ng，固定版本并校验 hash |
| 构建 | CMake 3.25+ 和 Native Assets hook |

## 下一步阅读

- [版本文档](/dart_cpp_bridge/zh-cn/versions/) — 选择 v1 或 v2；
- [快速开始](/dart_cpp_bridge/zh-cn/getting-started/) — 创建项目；
- [架构](/dart_cpp_bridge/zh-cn/guides/fundamentals/architecture/) — 调用流程；
- [标记选择](/dart_cpp_bridge/zh-cn/guides/fundamentals/markers/) — 选择 sync、async、normal 或 Stream；
- [v2 stdexec 异步 C++](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/) — sender 和 task 规则；
- [Channel 通道](/dart_cpp_bridge/zh-cn/guides/fundamentals/channels/) — oneshot、mpsc、背压和取消；
- [线程与阻塞任务](/dart_cpp_bridge/zh-cn/guides/fundamentals/threading/) — 线程池配置和自定义 scheduler；
- [内置运行时](/dart_cpp_bridge/zh-cn/guides/fundamentals/runtime/) — Runtime 和 scheduler；
- [生命周期管理](/dart_cpp_bridge/zh-cn/guides/fundamentals/lifecycle/) — session 和 finalizer；
- [Native Assets 构建 hook](/dart_cpp_bridge/zh-cn/guides/fundamentals/native-assets-hooks/) — 原生构建接入。
