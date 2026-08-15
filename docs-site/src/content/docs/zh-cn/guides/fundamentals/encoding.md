---
title: Wire 编码与运行时编解码
description: dart_cpp_bridge 的帧结构、payload 布局和运行时编解码流程
---

:::tip[本页的职责]
本页只说明二进制 frame、payload 的字节布局，以及 Dart/C++ 运行时如何读写
它们。C++ 类型如何映射成 Dart 类型、哪些类型能被 codegen 接受，请看
[类型映射](/dart_cpp_bridge/zh-cn/codegen/type-mapping/)；完整的固定帧规范请看
[Wire 协议](/dart_cpp_bridge/zh-cn/reference/wire-protocol/)。
:::

## 两层结构

一次调用由两层数据组成：

1. **Frame**：固定头部负责路由请求、响应、Stream 和 DartFn 消息。
2. **Payload**：由生成代码按参数声明顺序写入参数或返回值。

可以把它理解为：

```text
Dart generated API
        │  参数 / 返回值
        ▼
payload（ByteWriter / ByteReader）
        │  加上 24 字节固定头部
        ▼
wire frame
        │
        ▼
C++ dispatch / Session
```

通常不需要手写 `ByteWriter` 或 `ByteReader`：codegen 生成 C++ dispatch 和
Dart codec，业务代码只需要提供标记过的 C++ API。

## Frame 头部

固定头部为 24 字节，小端序排列：

| 偏移 | 大小 | 字段 | 用途 |
|---:|---:|---|---|
| 0 | 4 | `magic` | `0x31424344`（`DCB1`） |
| 4 | 2 | `version` | 当前为 `1` |
| 6 | 1 | `msg_type` | request、response、stream 或 DartFn |
| 7 | 1 | `flags` | 预留，目前为 `0` |
| 8 | 8 | `request_id` | RPC、Stream 或 DartFn 回复关联 ID |
| 16 | 4 | `method_id` | 生成的 API 方法编号 |
| 20 | 4 | `payload_len` | payload 字节数 |
| 24 | N | `payload` | 参数或结果数据 |

消息类型的完整列表和兼容约束见 [Wire 协议](/dart_cpp_bridge/zh-cn/reference/wire-protocol/)。
`method_id`、字段顺序和协议版本都属于兼容性约束，不能随意修改。

## Payload 的基础编码

payload 没有自描述的字段名或类型标签。发送方和接收方必须使用同一份生成
代码，以相同顺序读写：

| Wire 值 | 大小 / 布局 |
|---|---|
| `bool` | 1 字节，`0` 或 `1` |
| `u8` | 1 字节 |
| `i32` / `u32` | 4 字节小端序 |
| `i64` | 8 字节小端序 |
| `f32` / `f64` | IEEE 754 浮点数 |
| `string` | `u32` 字节长度 + UTF-8 字节，不含 `NUL` |
| `DateTime` | `i64` Unix 微秒时间戳，按 UTC 解释 |
| `Int128` / `UInt128` | `u32` 长度 + 十进制 ASCII 字符串 |
| 内部 `u64` | frame ID、对象 handle、指针地址等；不是普通 codegen 基础类型 |

公共 C++ 类型到这些 wire 值的映射，以[类型映射](/dart_cpp_bridge/zh-cn/codegen/type-mapping/)
为准。特别是，底层 codec 能表达的 `i16`、`u16`、`u64` 等值，不等于它们都
是 codegen 的公开 API 类型。

## 复合值的编码

- `std::vector<T>`：`u32` 元素个数，然后逐个写入元素。
- `std::vector<uint8_t>`：仍然有 `u32` 个数，后面直接写入字节。
- `std::array<T, N>`：固定写入 `N` 个元素，**不写元素个数**；Dart 生成代码
  会校验长度。
- `std::optional<T>`：`u8` presence tag；`0` 表示 `None`，`1` 后面紧跟 `T`。
- `std::map` / `std::unordered_map`：`u32` 个数，再写入每个 key/value。
- `std::set` / `std::unordered_set`：`u32` 个数，再写入每个元素。
- `std::pair` / `std::tuple`：按位置写入，不写长度或字段名。
- `enum class`：按 `i32` 写入枚举值。
- `BRIDGE_DATA_CLASS`：按 C++ 字段声明顺序递归写入，不写字段名。
- `BRIDGE_OPAQUE`：写入 `u64` 对象 handle，不写对象字段。
- `std::uint8_t*` / `const std::uint8_t*`：只写 `u64` native 地址，不写地址
  指向的字节，也不包含长度。长度必须作为另一个参数传递。

有序和无序 map/set 的 payload 结构相同；无序容器的迭代顺序不是 wire 协议
语义的一部分。

## 手写 codec 时的规则

只有在编写手写 wire dispatch、测试 fixture 或底层运行时时才需要直接使用
codec。写入和读取必须严格成对：

```cpp
dcb::ByteWriter writer;
writer.i32(42);
writer.str("Hi");

const auto& raw = writer.raw();
dcb::ByteReader reader(raw.data(), raw.size());
const auto number = reader.i32();  // 42
const auto text = reader.str();    // "Hi"
```

实际项目中，生成的 `wire_dispatch.cpp` 和 Dart `*.g.dart` 会完成同样的工作。
不要在业务函数中再包一层自定义 Future/Stream 或手动拼接 payload；异步函数
使用 `stdexec::task<T>`，Stream 使用 `dcb::StreamSink<T>`，类型和编码由
生成器统一处理。

## 一个 payload 示例

下面的值按顺序编码：

```text
int32 42       →  2A 00 00 00
string "Hi"    →  02 00 00 00 48 69
optional<int32>(42)
               →  01 2A 00 00 00
```

如果一个 API 的参数是 `std::vector<uint8_t>{10, 20}`，payload 片段为：

```text
02 00 00 00 0A 14
```

`std::array<int32_t, 2>{10, 20}` 则没有 `02 00 00 00` 这个长度前缀，因为
数组长度已经写在 C++ 类型中。

## 错误帧

C++ 异常不会跨过 FFI，而是在 wire 边界转换成 `responseErr`：

```text
code      i32     错误码（当前生成 dispatch 使用 1）
message   string  长度前缀的错误消息
```

Dart 侧收到错误帧后抛出对应异常；调用方不需要解析 C++ 异常对象。

## 校验与生命周期

- `parseFrame` / `ByteReader` 会检查 magic、协议版本、payload 长度和读取边界。
- wire 协议只用于同一进程内的 Dart 与捆绑 C++，不应当当作网络协议解析不可信
  输入。
- `uint8_t*` 只传地址。Dart 分配的输入 buffer 必须保持有效，直到同步调用
  返回或异步 `Future` 完成；C++ 必须自行校验地址和长度。
- 返回指针的所有权和有效期属于 C++ API 契约，bridge 不会自动复制、释放或
  添加边界检查。

## 延伸阅读

- [类型映射](/dart_cpp_bridge/zh-cn/codegen/type-mapping/)：C++ 类型、Dart 类型与 codegen 白名单
- [Wire 协议](/dart_cpp_bridge/zh-cn/reference/wire-protocol/)：协议字段和消息类型规范
- [stdexec 使用指南](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/)：异步业务代码的 sender/task 写法
- [代码生成](/dart_cpp_bridge/zh-cn/codegen/)：从 C++ 头文件生成 Dart/C++ binding
