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
| 1 | `kRequest` | Dart → C++ 请求 |
| 2 | `kResponseOk` | C++ → Dart 成功响应 |
| 3 | `kResponseErr` | C++ → Dart 错误响应 |
| 4 | `kStreamData` | Stream 数据帧 |
| 5 | `kStreamEnd` | Stream 结束 |
| 6 | `kStreamErr` | Stream 错误 |
| 7 | `kDartFnCall` | C++ → Dart 闭包调用 |

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
| `i8` / `u8` | 1 byte |
| `i16` / `u16` | 2 bytes little-endian |
| `i32` / `u32` | 4 bytes little-endian |
| `i64` / `u64` | 8 bytes little-endian |
| `f32` | 4 bytes IEEE 754 |
| `f64` | 8 bytes IEEE 754 |
| `string` | u32 length + UTF-8 bytes（无 NUL 终止） |
| `DateTime` | i64 Unix 微秒时间戳（UTC，不带时区） |
| `Int128` / `UInt128` | u32 length + 十进制 ASCII 字符串；Dart 负责与 `BigInt` 转换 |

### 枚举

底层类型为 `i32` 的 `enum class`，按 `i32` 编码。

### 容器类型

| 类型 | 编码 |
|---|---|
| `std::vector<T>` / `std::array<T, N>` | u32 count + T[] |
| `std::unordered_map<K, V>` | u32 count + (K, V)[] |
| `std::unordered_set<T>` | u32 count + T[] |
| `std::pair<T1, T2>` | T1 + T2 |
| `std::tuple<T1, T2, ...>` | T1 + T2 + ...（按位置） |
| `std::optional<T>` | u8 tag（1 = Some, 0 = None）+ T（仅 Some） |

## 安全考虑

:::caution
Wire 协议不设计用于解析来自网络的不可信数据。仅用于 Dart 和捆绑的 C++ 代码之间的通信。
:::

- 编解码器验证 `magic`、`version` 和 payload 长度
- 截断或格式错误的帧会抛出异常并编码为错误帧
- C++ 异常在 wire 边界被捕获，不会跨越 FFI
