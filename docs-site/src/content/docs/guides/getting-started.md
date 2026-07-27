---
title: 快速开始
description: 环境准备和第一次构建
---

## 环境要求

- CMake >= 3.24
- C++20 编译器 (MSVC 2019+, GCC 10+, Clang 12+)
- Dart SDK >= 3.10.0
- Git

## 构建基础库

```bash
# 1. 获取 Dart API DL 头文件
cmake -P dart/fetch_dart_api.cmake

# 2. 配置依赖 (asio/async-simple)
cmake -S dart/native -B dart/native/build -DCMAKE_BUILD_TYPE=Release

# 3. 构建 base_demo
cmake -S examples/base_demo -B examples/base_demo/build -DCMAKE_BUILD_TYPE=Release
cmake --build examples/base_demo/build --config Release

# 4. 运行 smoke 测试
./examples/base_demo/build/dcb_smoke        # Linux/macOS
./examples/base_demo/build/Release/dcb_smoke.exe  # Windows
```

## 运行 Dart 测试

```bash
cd dart
dart pub get
dart test
```

如果自动检测失败，设置库路径：

```bash
# PowerShell
$env:DCB_LIBRARY_PATH = "D:\path\to\dart_cpp_bridge.dll"
dart test

# Bash
DCB_LIBRARY_PATH=/path/to/libdart_cpp_bridge.so dart test
```

## Codegen Demo

```bash
# 1. 运行代码生成
cd dcb_gen_tool
dart pub get
dart run bin/dcb_gen.dart generate ../examples/codegen_demo/dart_cpp_bridge.yaml

# 2. 构建
cd ../examples/codegen_demo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 3. 测试
dart pub get
dart test
```
