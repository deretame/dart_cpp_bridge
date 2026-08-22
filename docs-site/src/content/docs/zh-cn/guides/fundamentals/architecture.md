---
title: 架构（v2）
description: dart_cpp_bridge 分层、调用流程和 v2 stdexec 运行时
---

:::note[v2.1.0]
以下架构描述当前 stdexec 实现。Dart API、C ABI、wire 协议和生成文件布局
仍与 v1 保持兼容。
:::

## 分层架构

```text
Dart / Flutter 应用
  └─ Dart 包（DartCppBridge、codec、FFI bindings）
       └─ FFI 二进制帧
原生库
  ├─ Runtime（Asio io_context + IoContextScheduler + blocking pool）
  ├─ Session registry（每个 Dart isolate 一个 session）
  ├─ Wire dispatch（帧路由和 method_id）
  ├─ Codec（ByteReader / ByteWriter）
  ├─ Channel 与 stdexec scheduler 适配器
  └─ 业务代码（BRIDGE_SYNC / BRIDGE_ASYNC / BRIDGE_NORMAL）
```

生成器扫描 `BRIDGE_*` 标记，并生成 wire dispatch、Dart FFI bindings 和
Dart API。v2 的异步 C++ 声明使用 `stdexec::task<T>` 或其他受支持的 sender。

## Runtime

进程级 `dcb::Runtime` 负责：

- 一个 Asio io 线程，并通过 `IoContextScheduler` 暴露；
- blocking Asio thread pool；
- 支持 scheduler 的通道和 timer；
- Dart post callback 与 session 生命周期。

生成的 task 具有 scheduler affinity。v2 不再把外部 loop 封装成 async-simple
executor；外部 loop 提供可以组合 sender 的普通 stdexec scheduler。

## 调用流程

### Sync

```text
Dart → dcb_invokeSyncMethod
     → io 线程解码请求
     → 调用 BRIDGE_SYNC 函数
     → 编码 responseOk / responseErr
     → 返回 Dart
```

函数必须短小且完全不阻塞。

### Async

```text
Dart → dcb_invokeAsyncMethod
     → 解码请求
     → 创建 stdexec::task<T>
     → starts_on(io_scheduler, task)
     → co_await sender / channel / timer / DartFn
     → 向 Dart session 发送 responseOk / responseErr
```

生成的协程 dispatch 使用零捕获 IIFE。参数进入协程帧，使 dispatch 函数返回
后状态仍然有效。

### Normal

```text
Dart → dcb_invokeNormalMethod
     → 普通 C++ 函数投递到 blocking pool
     → 编码结果或异常
     → 向 Dart 发送 response
```

### Stream 与 DartFn

Stream 使用 `StreamSink<T>` 发送 `streamData`、`streamEnd`、
`streamErr` 帧。Dart 取消订阅只停止接收，原生操作可能继续运行，
之后的 sink 调用会被丢弃。

`DartFn` 调用发送 `dartFnCall` 帧并等待 oneshot sender。Dart 执行闭包
期间 io 线程挂起，收到 reply 后再恢复。

## 线程模型

- **io 线程**：帧 dispatch、scheduler 工作、非阻塞 timer 和发起 DartFn；
- **blocking pool**：`BRIDGE_NORMAL` 和 `spawn_blocking` 工作；
- **外部 loop 线程**：用户提供的 stdexec scheduler；
- **Dart isolate**：Dart 代码和闭包执行。

不要阻塞 io 线程。只在 worker 或外部线程使用 `dcb::sync_wait`，取消使用
stop token 做协作式传播。

## 延伸阅读

- [v2 stdexec 异步 C++](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/)
- [内置运行时](/dart_cpp_bridge/zh-cn/guides/fundamentals/runtime/)
- [Wire 协议](/dart_cpp_bridge/zh-cn/reference/wire-protocol/)
- [外部运行时集成](/dart_cpp_bridge/zh-cn/guides/advanced/foreign-runtime/)
