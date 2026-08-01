---
title: 项目结构
description: 一个典型 dart_cpp_bridge 项目的目录布局
---

`dcb_gen_tool init` 会生成一个基础项目骨架。了解哪些文件是手写的、哪些是生成的，能避免改错文件。

## 典型布局

```text
my_project/
├── dart_cpp_bridge.yaml      # codegen 配置
├── pubspec.yaml              # Dart/Flutter 包配置
├── hook/
│   └── build.dart            # Native Assets build hook
├── native/
│   ├── api/
│   │   └── my_api.h          # 手写 C++ API 头文件（BRIDGE_* 标记）
│   ├── api_impl/
│   │   └── my_api.cpp        # 手写业务实现
│   ├── CMakeLists.txt        # 手写/调整的构建配置
│   └── generated/
│       ├── wire_dispatch.hpp # 生成
│       └── wire_dispatch.cpp # 生成
└── lib/
    ├── src/
    │   └── native_gen/
    │       ├── dcb_bindings.dart      # 生成：FFI 函数签名
    │       ├── dcb_generated.dart     # 生成：内部实现/单例
    │       └── api/
    │           └── my_api.dart        # 生成：顶层调用入口
    └── my_project.dart       # 手写：导出 public API
```

## 手写 vs 生成

| 路径 | 类型 | 说明 |
|---|---|---|
| `dart_cpp_bridge.yaml` | 手写 | 配置 source dir、生成目录、lib 名等 |
| `pubspec.yaml` | 手写 | Dart/Flutter 依赖 |
| `hook/build.dart` | 手写 | Native Assets 构建入口 |
| `native/api/*.h` | 手写 | 桥接 API 头文件，包含 `BRIDGE_*` 标记 |
| `native/api_impl/*.cpp` | 手写 | 业务实现 |
| `native/CMakeLists.txt` | 手写/调整 | 构建 native 库 |
| `native/generated/*` | 生成 | wire dispatch 路由 |
| `lib/src/native_gen/*` | 生成 | FFI 绑定、Dart 实现 |
| `lib/<project>.dart` | 手写 | 导出需要暴露的 Dart API |

## 生成命令

修改 `native/api/*.h` 后，运行：

```bash
dcb_gen_tool generate
```

或在 `dcb_gen_tool` 源码目录里：

```bash
cd dcb_gen_tool
dart run bin/dcb_gen_tool.dart generate ../path/to/dart_cpp_bridge.yaml
```

## 注意事项

- `native/api/*.h` 及其传递包含的所有头文件都会参与 codegen 解析。被扫描的
  头文件只保留 include 白名单（C++ 标准库、`dart_cpp_bridge/*`、
  `async_simple/coro/Lazy.h`），实现和三方库 include 放在
  `native/api_impl/*.cpp`。参见
  [配置 → 头文件组织](/dart_cpp_bridge/codegen/configuration/)。
- 不要手动修改 `native/generated/` 和 `lib/src/native_gen/` 里的文件，重新生成会覆盖
- 如果生成输出路径不满意，可以在 `dart_cpp_bridge.yaml` 里调整
- 业务代码应放在 `native/api_impl/` 或 `native/api/` 中，保持生成层不依赖具体实现

## 延伸阅读

- [注意事项与常见坑](./caveats/)
- [代码生成配置](/dart_cpp_bridge/codegen/configuration/)
- [代码生成输出](/dart_cpp_bridge/codegen/output/)
