---
title: Wire 协议
description: FFI 二进制帧格式规范
---

## 帧格式

所有 Dart ↔ C++ 通信使用小端二进制帧：

```text
偏移  大小  字段        说明
0     4     magic       0x31424344 ('DCB1')
4     2     version     协议版本 (当前 1)
6     1     msg_type    消息类型
7     1     flags       保留 (0)
8     8     request_id  RPC ID / Stream ID / DartFn Reply ID
16    4     method_id   方法标识
20    4     payload_len 负载长度
24    N     payload     负载数据
```

## 消息类型

| 值 | 名称 | 说明 |
|---|---|---|
| 0 | `request` | Dart → C++ 请求 |
| 1 | `responseOk` | C++ → Dart 成功响应 |
| 2 | `responseErr` | C++ → Dart 错误响应 |
| 3 | `streamData` | Stream 数据帧 |
| 4 | `streamEnd` | Stream 结束 |
| 5 | `streamErr` | Stream 错误 |
| 6 | `dartFnCall` | C++ → Dart 闭包调用 |

## 错误编码

错误响应的 payload 格式：

```text
code      i32     错误码
message   string  错误消息 (长度前缀)
```

## 类型编码

### 基本类型

| 类型 | 编码 |
|---|---|
| `bool` | 1 byte (0/1) |
| `i32` | 4 bytes little-endian |
| `i64` | 8 bytes little-endian |
| `f64` | 8 bytes IEEE 754 |
| `string` | u32 length + UTF-8 bytes |

### 容器类型

| 类型 | 编码 |
|---|---|
| `Vec<T>` | u32 count + T[] |
| `Map<K,V>` | u32 count + (K, V)[] |
| `Set<T>` | u32 count + T[] |

## 安全考虑

:::caution
Wire 协议不设计用于解析来自网络的不可信数据。仅用于 Dart 和捆绑的 C++ 代码之间的通信。
:::

- 编解码器验证 `magic`、`version` 和 payload 长度
- 截断或格式错误的帧会抛出异常并编码为错误帧
- C++ 异常在 wire 边界被捕获，不会跨越 FFI
