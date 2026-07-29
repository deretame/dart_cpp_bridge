---
title: 架构设计
description: 系统架构和核心组件
---

## 核心组件

### Runtime

进程级单例，管理：
- `asio::io_context` 单线程事件循环
- `asio::thread_pool` 工作线程池
- Session 注册表

### Session

每个 Dart Isolate 对应一个 Session：
- 独立的 reply port
- DartFn 闭包注册表
- 生命周期由 NativeFinalizer 管理

### Wire 协议

二进制帧格式（小端）：

```text
magic       u32   0x31424344 ('DCB1')
version     u16   1
msg_type    u8    request / responseOk / responseErr / ...
flags       u8    reserved
request_id  u64   RPC id / stream id
method_id   u32   方法标识
payload_len u32
payload     bytes
```

## 调用流程

### 同步调用

```text
Dart → FFI → wire dispatch → C++ function → wire response → Dart
```

### 异步调用

```text
Dart → FFI → wire dispatch → spawn Lazy on io_context
                                    ↓
Dart ← port message ← wire response ← coroutine complete
```

### Stream

```text
Dart subscribe → FFI → C++ StreamSink
                            ↓ add()
Dart ← streamData frame ← port
                            ↓ close()
Dart ← streamEnd frame ← port
```

## 线程模型

:::caution
永远不要阻塞 `io_context` 线程。阻塞操作必须使用 `spawn_blocking` 或 `thread_pool`。
:::

- **io_context 线程**: 事件循环、协程调度、Dart API 调用
- **thread_pool**: 阻塞业务逻辑
- **Dart Isolate**: 独立的 Dart 执行环境

## 外部运行时集成

bridge 支持将非 asio 事件循环（libuv、glib 等）通过 `ForeignExecutor` 适配层接入协程系统，实现跨运行时非阻塞通信和 Dart 回调调用。

详见 [外部运行时集成](foreign-runtime/)。

对于纯 C 代码或不依赖 async-simple 的场景，提供 [纯 C 桥接 API](cbridge/)（callback 风格，零 C++ 依赖）。
