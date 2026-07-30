---
title: C++ ↔ Dart 类型翻译
description: C++ 类型与 Dart 类型的互相翻译表，以及背后的 wire 编码规则速查
---

这页是一张**类型翻译速查表**：你写 C++ API 时，应该知道 Dart 侧看到什么类型；你在 Dart 里调用生成的函数时，也应该知道 C++ 侧实际收发什么类型。完整规则见 [类型映射](/dart_cpp_bridge/codegen/type-mapping/) 和 [Wire 协议](/dart_cpp_bridge/reference/wire-protocol/)。

## C++ ↔ Dart 类型

| C++ 类型 | Dart 类型 | 编码 |
|---|---|---|
| `bool` | `bool` | 1 byte (0/1) |
| `int8/16/32/64_t` | `int` | 1/2/4/8 bytes LE |
| `uint8/16/32/64_t` | `int` | 1/2/4/8 bytes LE |
| `float` | `double` | 4 bytes IEEE 754 |
| `double` | `double` | 8 bytes IEEE 754 |
| `std::string` | `String` | u32 length + UTF-8 bytes |
| `std::chrono::system_clock::time_point` | `DateTime` | i64 微秒时间戳 |
| `Int128` / `UInt128` | `BigInt` | 长度前缀十进制字符串 |

## 容器

| C++ 类型 | Dart 类型 | 编码 |
|---|---|---|
| `std::vector<uint8_t>` | `Uint8List` | u32 count + bytes |
| `std::vector<int32_t>` | `Int32List` | u32 count + elements |
| `std::vector<float>` | `Float32List` | u32 count + elements |
| `std::vector<double>` | `Float64List` | u32 count + elements |
| `std::vector<T>`（其他） | `List<T>` | u32 count + N 个元素 |
| `std::optional<T>` | `T?` | 1 byte tag (0=None, 1=Some) + T |
| `std::unordered_map<K, V>` | `Map<K, V>` | u32 count + (K, V) pairs |
| `std::unordered_set<T>` | `Set<T>` | u32 count + N 个元素 |
| `std::pair<T1, T2>` | `(T1, T2)` | 顺序编码 |
| `std::tuple<T1, T2, ...>` | `(T1, T2, ...)` | 顺序编码 |

## 枚举

```cpp
enum class BRIDGE_EXPORT OrderStatus : std::int32_t {
  kCreated = 0,
  kPaid = 1,
};
```

- 必须标 `BRIDGE_EXPORT`
- 底层类型必须是 `std::int32_t`
- 每个常量必须显式赋值
- wire 编码为 `int32`

## 数据类

```cpp
struct BRIDGE_DATA_CLASS Point {
  double x;
  double y;
};
```

- 按 **声明顺序** 逐字段编码
- 不传输字段名
- 不支持继承、虚函数、方法

## 不透明类

`BRIDGE_OPAQUE` 对象在 wire 上按 handle 传递，不是按字段编码。

## 编码约定

- **小端序**：所有多字节整数都是 little-endian
- **顺序敏感**：Reader 是顺序游标，读取顺序必须和写入顺序完全一致
- **字符串不 NUL 结尾**：`std::string` 传输的是 u32 length + 字节，不含 `\0`
- **无字段名**：数据类只传值，不传字段名

## 错误帧

`responseErr` 的 payload：

```text
code    i32    目前恒为 0
message string 错误信息
```

## 延伸阅读

- [类型映射](/dart_cpp_bridge/codegen/type-mapping/)
- [Wire 协议](/dart_cpp_bridge/reference/wire-protocol/)
- [纯 C 桥接 API](/dart_cpp_bridge/guides/advanced/cbridge/)
