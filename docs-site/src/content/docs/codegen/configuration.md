---
title: 配置
description: dart_cpp_bridge.yaml 配置文件说明
sidebar:
  order: 1
---

## 配置文件

每个需要代码生成的项目需要一个 `dart_cpp_bridge.yaml`：

```yaml
# dart_cpp_bridge.yaml
header: native/api/bridge_api.h
output:
  cpp: native/generated
  dart: lib/src/generated
include_paths:
  - native
  - native/api
  - ../../dart/native/include
  - ../../dcb_gen_tool/stubs
```

## 字段说明

| 字段 | 必填 | 说明 |
|------|------|------|
| `header` | ✅ | 要解析的 C++ 头文件路径 |
| `output.cpp` | ✅ | C++ 生成代码输出目录 |
| `output.dart` | ✅ | Dart 生成代码输出目录 |
| `include_paths` | ✅ | 头文件搜索路径列表 |
| `dart_code` | ❌ | 按类名注入自定义 Dart 代码 |

## include_paths 注意事项

:::tip
确保 `include_paths` 包含所有必要的头文件路径，特别是：
- 项目自身的头文件目录
- `dart/native/include`（运行时头文件）
- `dcb_gen_tool/stubs`（解析用 stub 头文件）
:::

## 运行代码生成

```bash
cd dcb_gen_tool
dart pub get
dart run bin/dcb_gen.dart generate ../path/to/dart_cpp_bridge.yaml
```

首次运行会自动下载并缓存 Python + libclang 工具链。
