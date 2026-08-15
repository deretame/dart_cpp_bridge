---
title: 配置
description: dart_cpp_bridge.yaml 配置文件说明
sidebar:
  order: 1
---

:::caution[v2 开发线]
当前生成器解析的异步返回类型是 `stdexec::task<T>`。已发布的 v1 生成器使用
async-simple；请让生成器和 Runtime 使用同一 revision。
:::

## 配置文件

每个需要代码生成的项目需要一个 `dart_cpp_bridge.yaml`，放在项目根目录：

```yaml
# dart_cpp_bridge.yaml
dart_package: my_app

cpp_root: native/

scan:
  - native/api/

# 项目自身的 include 路径即可，dart_cpp_bridge 的 native/include 目录会在 codegen 时
# 从 .dart_tool/package_config.json 自动解析，不需要写在这里。
include_paths:
  - native
  - native/api

dart_output: lib/src/native_gen/
cpp_wire_output: native/generated/

# 可选：clang-format 候选路径（从上到下尝试，最后回退到 PATH）。
# 可以是可执行文件路径，也可以是包含可执行文件的目录。
# clang_format:
#   - C:\Program Files\LLVM\bin

std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
```

## 字段说明

| 字段 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `cpp_root` | ✅ | `native/` | C++ 源码根目录，`scan` 路径相对于它解析 |
| `scan` | ✅ | - | 扫描注解头文件的目录列表（相对于 `cpp_root`） |
| `include_paths` | ✅ | `[]` | 传给 clang 的项目相对 include 路径 |
| `dart_output` | ✅ | `lib/src/native_gen/` | 生成的 Dart 代码输出目录 |
| `cpp_wire_output` | ✅ | `native/generated/` | 生成的 C++ wire dispatch 输出目录 |
| `dart_package` | ❌ | 从 `pubspec.yaml` 自动读取 | Dart 包名，用于生成 `assetId` 和 import 路径；手动写时必须与 `pubspec.yaml` 的 `name` 一致 |
| `clang_format` | ❌ | 无 | `clang-format` 候选路径列表，用于格式化生成的 C++ 代码 |
| `std` | ❌ | `c++20` | 解析 C++ 头文件时使用的 C++ 标准 |
| `defines` | ❌ | `BRIDGE_CODEGEN`、`DART_CPP_BRIDGE_CODEGEN` | 传给 clang 的预处理宏列表 |
| `dart_code` | ❌ | 无 | 向生成的数据类中注入自定义 Dart 代码，可替换自动生成的 `toString()` |

## 运行代码生成

```bash
# 推荐：安装到 $DART_DATA_HOME/install/bin/ 后使用
dart install dcb_gen_tool
dcb_gen_tool generate dart_cpp_bridge.yaml

# 旧版全局激活方式（仍可用）
dart pub global activate dcb_gen_tool
dcb_gen_tool generate dart_cpp_bridge.yaml
```

首次运行会自动下载并缓存 Python + libclang 工具链。

## include_paths 注意事项

:::tip
`include_paths` 只需要写项目自身的头文件目录。`dart_cpp_bridge` 的运行时头文件和 `dcb_gen_tool` 的 stub 头文件会在 codegen 时自动解析，不需要手动添加。
:::

## 头文件组织建议

`dcb_gen_tool` 使用 libclang 解析被扫描的头文件——**包括它们传递包含的所有
头文件**。libclang 解析不了的头文件不一定会让生成过程报错，而是可能把无法
解析的模板类型（如 `std::vector<T>`、`std::unordered_map<K,V>`）静默降级为
`int`，生成的绑定要到 C++/Dart 编译阶段才暴露问题。

### include 白名单

`native/api/*.h` 只允许 include：

- C++ 标准库头文件（用于签名中的类型）；
- `dart_cpp_bridge/*` 运行时头文件；
- `BRIDGE_ASYNC` 返回类型需要的 `stdexec/execution.hpp`（vendored 依赖不可用时由
  `dcb_gen_tool` 的 stubs 兜底）。

**不要**在被扫描的头文件中 include 其他三方库或项目依赖头文件——包括只存在于
构建环境中的头（`build/_deps`、内置 SDK 等）。重型 include 和实现请放到
`native/api_impl/*.cpp`，codegen 不会解析该目录。

### 声明规范

- 被 `scan` 扫描到的头文件应只放**声明**，不要写函数实现（不要把 `.cpp` 内容贴进头文件）。
- **数据类**和**不透明类**必须直接定义在头文件内；codegen 只解析被扫描的头文件，不会解析头文件外部定义的类。
- 自由函数、静态方法、构造函数等实现请放在对应的 `.cpp` 文件中。
- **不要写类型别名**（`using Foo = ...` 或 `typedef ...`），codegen 目前无法解析别名，直接展开为实际类型。
- **不要写 `using namespace`**，所有类型和函数调用都应写完整命名空间（如 `std::int32_t`、`stdexec::task`）。
- 函数/方法参数和返回值类型请使用完整限定名，确保 codegen 能正确识别。

## 自定义数据类 `toString()` {#dart_code}

对于数据类，codegen 默认生成 `hashCode`、`operator ==` 和 `toString()`。如果你希望某个数据类使用自定义的 `toString()`，可以通过 `dart_code` 注入：

```yaml
dart_code:
  Rect: |
    @override
    String toString() => 'Rect[$topLeft -> $bottomRight]';
```

规则：
- 键为数据类类名（必须与 C++ 中 `BRIDGE_DATA_CLASS` 的类名一致）
- 注入的代码会原样写入生成的 Dart 类 body 中
- 当某个类存在 `dart_code` 时，会**替换**自动生成的 `toString()`，但 `hashCode` 和 `operator ==` 仍然保留

更多关于数据类字段类型限制，参见 [类型映射 → 数据类](../type-mapping/#数据类-data-class)。
