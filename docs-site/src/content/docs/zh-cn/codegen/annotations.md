---
title: 注解标记
description: C++ 头文件中的 BRIDGE_* 宏标记
sidebar:
  order: 2
---

:::caution[v2 开发线]
`BRIDGE_ASYNC` 在 v2 头文件中返回 `stdexec::task<T>` 或其他受支持的
stdexec sender。已发布的 v1 项目复制异步声明前，请先查看[版本说明](/dart_cpp_bridge/zh-cn/versions/)。
:::

## 概述

在 C++ 头文件中使用注解标记来指示代码生成器如何处理函数和类。

这些宏在 `BRIDGE_CODEGEN` 定义时展开为 `__attribute__((annotate("bridge::*")))`，否则展开为空。

## 重要命名约束

所有导出到 Dart 的函数及不透明类方法，在扫描到的 API 范围内必须具有**全局唯一的限定名**。C++ 函数重载**不被支持**。

### 原因

桥接层会为每个导出的 API 根据其完整限定名生成一个稳定的整数方法 ID，Dart 端通过这个 ID 进行调用分发。如果两个函数共享同一个限定名，生成器无法区分它们；同时 Dart 也没有与 C++ 重载解析等价的机制。

### 规则

- 不能声明两个限定名相同的 `BRIDGE_SYNC`、`BRIDGE_ASYNC` 或 `BRIDGE_NORMAL` 函数。
- 不透明类的方法在同一类内也必须唯一。即使签名不同，同一个 `Counter` 类中也不能有两个名为 `process` 的方法。
- 若 C++ 端存在重载，请在暴露给桥接层之前重命名，例如：

  ```cpp
  BRIDGE_SYNC int32_t add_ints(int32_t a, int32_t b);
  BRIDGE_SYNC double add_doubles(double a, double b);
  ```

违反该约束会导致代码生成器报重复函数错误并中止。

## 函数注解

### BRIDGE_SYNC

同步函数，直接返回结果：

```cpp
BRIDGE_SYNC int32_t add(int32_t a, int32_t b);
```

### BRIDGE_ASYNC

异步函数，返回 `stdexec::task<T>` 或其他受支持的 sender：

```cpp
#include <stdexec/execution.hpp>
BRIDGE_ASYNC stdexec::task<int32_t> compute_async(int32_t input);
```

### BRIDGE_NORMAL

普通函数，投递到线程池执行：

```cpp
BRIDGE_NORMAL std::string blocking_read(std::string path);
```

### Stream 函数

带必需 `dcb::StreamSink<T>` 参数的函数会生成 Dart `Stream<T>`，但前提是它还带有导出标记
（`BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL`）。普通 `void` stream 函数用
`BRIDGE_NORMAL`：

```cpp
BRIDGE_NORMAL
void tick_stream(dcb::StreamSink<int32_t> sink, int32_t count);
```

约束：

- 导出标记是门槛：只有 `StreamSink` 参数而没有导出标记的函数**不会**生成
  （生成器会告警并跳过）
- 可选 stream 用 `BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL` 函数上的
  `std::optional<dcb::StreamSink<T>>` 参数（sync 的事件在 FFI 调用返回后送达）

### BRIDGE_PERSIST

标记含 DartFn 参数的函数为「持久化回调」：Dart 侧不在调用后自动注销闭包，允许 C++ 存储并反复调用。通常与 `BRIDGE_SYNC`（注册）或 `BRIDGE_NORMAL`（触发）配合使用：

```cpp
BRIDGE_SYNC
BRIDGE_PERSIST
bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback);
```

约束：
- 函数必须含至少一个 `dcb::DartFn` 参数
- 回调不会自动清理，调用者需自行管理生命周期

## 类注解

### BRIDGE_DATA_CLASS

纯数据类（仅字段，无导出方法）：

```cpp
struct BRIDGE_DATA_CLASS Point {
  double x;
  double y;
};
```

约束：
- 无继承
- 无虚函数
- 无 `BRIDGE_SYNC/ASYNC/NORMAL` 方法

### BRIDGE_OPAQUE

不透明类（仅方法，公共字段被忽略）：

```cpp
class BRIDGE_OPAQUE Counter {
 public:
  BRIDGE_SYNC void increment();
  BRIDGE_SYNC int32_t value() const;
 private:
  int32_t count_ = 0;
};
```

### BRIDGE_TO_STRING

标记不透明类方法作为 Dart `toString()` 的来源：

```cpp
class BRIDGE_OPAQUE Widget {
 public:
  BRIDGE_SYNC BRIDGE_TO_STRING std::string to_string() const;
};
```

约束：
- 必须是同步实例方法
- 无参数
- 返回 `std::string`

## 别名

所有 `BRIDGE_*` 宏都有 `DCB_*` 别名：

```cpp
DCB_SYNC == BRIDGE_SYNC
DCB_ASYNC == BRIDGE_ASYNC
DCB_DATA_CLASS == BRIDGE_DATA_CLASS
// ...
```
