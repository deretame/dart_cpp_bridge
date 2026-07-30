---
title: 生成产物
description: 代码生成器输出的文件结构
sidebar:
  order: 4
---

## C++ 侧

```text
native/generated/
├── wire_dispatch.hpp   # 分发函数声明
├── wire_dispatch.cpp   # 分发实现（帧解码、方法路由、编码响应）
└── ir.json             # 中间表示（调试用）
```

### wire_dispatch.cpp

生成的分发代码处理：
- 帧解码（`ByteReader`）
- 方法路由（`switch (method_id)`）
- 参数反序列化
- 调用业务函数
- 返回值序列化
- 错误捕获和编码

## Dart 侧

```text
lib/src/generated/
├── api_fn.dart    # 顶层函数（推荐调用入口）
├── api.dart       # BridgeApi 单例
└── api.g.dart     # BridgeApiImpl（方法 ID、编解码）
```

### 三层结构

| 层 | 文件 | 用途 |
|---|---|---|
| 顶层函数 | `api_fn.dart` | `initBridge()`, `add()`, ... |
| 单例 | `api.dart` | `BridgeApi.instance` |
| 实现 | `api.g.dart` | 方法 ID、编解码逻辑 |

### 使用示例

```dart
import 'package:my_app/src/generated/api_fn.dart';

void main() async {
  await initBridge();
  
  final result = await add(1, 2);
  print(result); // 3
  
  disposeBridge();
}
```

## 业务代码

业务实现保持在用户编写的文件中：

```text
native/api_impl/bridge_api.cpp  # 用户手写实现
```

代码生成器不会修改这些文件。
