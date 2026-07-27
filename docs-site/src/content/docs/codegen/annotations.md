---
title: 注解标记
description: C++ 头文件中的 BRIDGE_* 宏标记
sidebar:
  order: 2
---

## 概述

在 C++ 头文件中使用注解标记来指示代码生成器如何处理函数和类。

这些宏在 `BRIDGE_CODEGEN` 定义时展开为 `__attribute__((annotate("bridge::*")))`，否则展开为空。

## 函数注解

### BRIDGE_SYNC

同步函数，直接返回结果：

```cpp
BRIDGE_SYNC int32_t add(int32_t a, int32_t b);
```

### BRIDGE_ASYNC

异步函数，返回 `Lazy<T>`：

```cpp
BRIDGE_ASYNC async_simple::coro::Lazy<int32_t> compute_async(int32_t input);
```

### BRIDGE_NORMAL

普通函数（不暴露给 Dart，仅内部使用）：

```cpp
BRIDGE_NORMAL void internal_helper();
```

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

### BRIDGE_EXPORT (遗留)

自动检测：有导出方法 → opaque，否则 → data_class。

## 别名

所有 `BRIDGE_*` 宏都有 `DCB_*` 别名：

```cpp
DCB_SYNC == BRIDGE_SYNC
DCB_ASYNC == BRIDGE_ASYNC
DCB_DATA_CLASS == BRIDGE_DATA_CLASS
// ...
```
