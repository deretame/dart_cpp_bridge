---
title: 生成产物
description: 代码生成器输出的文件结构
sidebar:
  order: 4
---

## C++ 侧

默认输出到 `native/generated/`（由 `dart_cpp_bridge.yaml` 的 `cpp_wire_output` 配置）：

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

默认输出到 `lib/src/native_gen/`（由 `dart_cpp_bridge.yaml` 的 `dart_output` 配置）：

```text
lib/src/native_gen/
├── api/
│   ├── init.dart              # 初始化 / dispose / BridgeApi 单例
│   ├── bridge_api.dart        # 对应 native/api/bridge_api.h
│   ├── counter.dart           # 对应 native/api/counter.h
│   ├── foreign_api.dart       # 对应 native/api/foreign_api.h
│   └── multi_runtime_api.dart # 对应 native/api/multi_runtime_api.h
├── dcb_bindings.dart          # FFI 原生符号绑定
└── dcb_generated.dart           # 方法 ID、通用编解码、内部实现
```

即：`native/api/{name}.h` 会生成 `lib/src/native_gen/api/{name}.dart`。

### 入口文件

推荐通过包根目录的导出文件使用：

```dart
import 'package:codegen_demo/codegen_demo.dart';
```

`lib/codegen_demo.dart` 会把 `api/` 下的文件统一 export 出来。

### 三层结构

每个 API 头文件生成的 Dart 文件内部仍保持三层：

| 层 | 位置 | 用途 |
|---|---|---|
| 顶层函数 | `api/{name}.dart` | `initBridge()`, `add()`, ... |
| 单例 | `api/init.dart` 等 | `BridgeApi.instance` |
| 实现 | `dcb_generated.dart` | 方法 ID、编解码逻辑 |

### 使用示例

```dart
import 'package:my_app/codegen_demo.dart';

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
