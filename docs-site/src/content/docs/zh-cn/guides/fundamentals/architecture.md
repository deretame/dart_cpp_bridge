---
title: 架构设计
description: dart_cpp_bridge 分层架构、核心组件与调用流程
---

## 分层架构

```text
Dart / Flutter App
  └─ dart_cpp_bridge Dart package (DartCppBridge, codec, FFI bindings)
       └─ FFI binary frames (wire protocol)
Native library
  ├─ Runtime (asio io_context + AsioExecutor + thread_pool)
  ├─ Session registry (per-Isolate reply port, DartFn closures)
  ├─ Wire dispatch (frame routing, method_id → user code)
  ├─ Codec (ByteReader/Writer, frame + payload 编解码)
  ├─ Channel / ForeignExecutor / Cbridge adapters
  └─ User code (BRIDGE_SYNC / BRIDGE_ASYNC / BRIDGE_NORMAL functions)
```

代码生成工具 `dcb_gen_tool` 根据用户头文件中的 `BRIDGE_*` 标记生成 wire dispatch、Dart FFI 绑定和 Dart API 层。

## 核心组件

### Runtime

进程级单例，管理：

- `asio::io_context` 单线程事件循环
- `dcb::AsioExecutor` 把 async-simple 协程调度到 `io_context`
- `asio::thread_pool` 阻塞工作池
- Session 注册表

业务代码可以直接使用 Runtime 提供的基础工具：`spawn`、`spawn_blocking`、`spawn_detached`、`channel`、`sleep` 等。详见 [基础运行时](/dart_cpp_bridge/guides/fundamentals/runtime/)。

### Session

每个调用 `DartCppBridge.init()` 的 Dart Isolate 对应一个 Session：

- 独立的长期 reply port
- DartFn 闭包注册表（`fn_id` → closure）
- generation 计数器，dispose 后晚到的消息会被丢弃
- 生命周期由 `NativeFinalizer` 管理，Dart 对象不可达或 isolate 退出时自动关闭

### Wire Dispatch

由 `dcb_gen_tool` 生成或手写。它负责：

- 从 Dart 接收二进制帧，校验 `magic` / `version` / payload 长度
- 按 `method_id` 路由到用户 C++ 函数
- 捕获用户代码抛出的 `std::exception`，编码为 `responseErr` 帧返回 Dart
- 把 sync / async / stream / DartFn 调用转发到对应处理路径

### Codec

底层二进制帧格式见 [Wire 协议](/dart_cpp_bridge/reference/wire-protocol/)。`ByteReader` / `ByteWriter` 负责 payload 中基础类型、字符串、容器、可选类型、数据类等的编解码。两侧必须严格按同一顺序读写。

### ObjectHandleRegistry（不透明类）

`BRIDGE_OPAQUE` 标记的 C++ 对象按值传递：每实例分配一个 handle，存入 per-Session 的 `ObjectHandleRegistry`。Dart 侧通过 handle 调用实例方法，Dart GC 时通过 `NativeFinalizer` 调用 `dcb_drop_object` 释放 C++ 对象。

## 调用流程

### 同步调用（BRIDGE_SYNC）

```text
Dart 侧函数调用
  → FFI dcb_invokeSyncMethod
  → wire dispatch 解码参数
  → 用户 C++ 函数（在 io_context 线程同步执行，不能阻塞）
  → 编码返回值
  → responseOk 帧
  → Dart 同步返回
```

### 异步调用（BRIDGE_ASYNC）

```text
Dart 侧 Future 调用
  → FFI dcb_invokeAsyncMethod
  → wire dispatch 解码参数
  → 在 io_context 线程上启动 async_simple::Lazy<T> 协程
  → 协程可 co_await 挂起（不占线程）
  → 协程完成
  → responseOk / responseErr 帧 post 到 Dart reply port
  → Dart Completer 完成
```

### 普通调用（BRIDGE_NORMAL）

```text
Dart 侧 Future 调用
  → FFI dcb_invokeNormalMethod
  → wire dispatch 把阻塞任务投递到 thread_pool
  → 用户函数在线程池执行（可阻塞）
  → 编码结果
  → responseOk / responseErr 帧 post 到 Dart reply port
  → Dart Completer 完成
```

### Stream

```text
Dart subscribe
  → FFI 创建 C++ StreamSink
  → C++ 侧 sink.add(item) 发送 streamData 帧
  → Dart StreamController 收到数据
  → C++ 侧 sink.end() 发送 streamEnd 帧
  → Dart Stream 结束
```

取消订阅仅停止 Dart 侧接收；C++ 侧继续运行，后续 `add()` 会被静默丢弃。

### DartFn 反向调用

```text
Dart 把闭包作为参数传给 C++
  → C++ 保存 fn_id
  → 需要时 C++ 发送 DartFnCall 帧到 Dart
  → Dart 执行闭包
  → Dart 发送 DartFnReply 帧
  → C++ oneshot channel 完成
  → 等待中的协程恢复
```

`co_await callback(args)` 在 io 线程上真挂起，不占用线程池。

## 线程模型

:::caution
永远不要阻塞 `io_context` 线程。阻塞操作必须使用 `dcb::spawn_blocking` 或 `thread_pool`。
:::

- **io_context 线程**：事件循环、协程调度、Dart 帧收发、DartFn 回调触发
- **thread_pool**：`BRIDGE_NORMAL` 和 `spawn_blocking` 的阻塞业务逻辑
- **外部运行时线程**：通过 `ForeignExecutor` 接入的非 asio 事件循环线程
- **Dart Isolate 线程**：Dart 代码执行和 Dart 闭包执行环境

跨线程/跨运行时通信优先使用 `co::oneshot` / `co::mpsc` channel，而不是裸锁 + 条件变量。

## 外部运行时与纯 C 接入

bridge 支持将非 asio 事件循环（libuv、glib、自定义 loop 等）通过 `ForeignExecutor` 适配层接入协程系统，实现跨运行时非阻塞通信和 Dart 回调调用。详见 [外部运行时集成](/dart_cpp_bridge/guides/advanced/foreign-runtime/)。

对于纯 C 代码或不依赖 async-simple 的场景，提供 [纯 C 桥接 API](/dart_cpp_bridge/guides/advanced/cbridge/)（callback 风格，零 C++ 依赖）。

## 代码生成产物

对一个 `dcb_gen_tool` 项目，典型生成产物包括：

- `native/generated/wire_dispatch.{hpp,cpp}` — 路由与编解码
- `lib/src/native_gen/dcb_bindings.dart` — FFI 函数签名
- `lib/src/native_gen/api/{api}.dart` — 顶层函数调用入口
- `lib/src/native_gen/dcb_generated.dart` — 内部实现（impl + 单例）

业务实现仍保留在用户手写的 `.cpp` 文件中，生成层只负责把 Dart 调用路由到正确的 C++ 函数。
