# dcb_gen_tool

[dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge) 的代码生成 CLI 工具。

通过 libclang 解析带注解的 C++ 头文件，自动生成 Dart/C++ 桥接代码
（同步、异步、流、DartFn 反向调用、opaque 类）。

## 安装

```bash
dart pub global activate dcb_gen_tool
```

仅需 Dart SDK >= 3.10.0。无需安装 Python、LLVM 或 Rust ——
工具会自动下载经过 SHA-256 校验的固定版本 Python 工具链。

## 快速开始

```bash
# 1. 创建 Dart 项目并添加 dart_cpp_bridge 依赖后：
cd my_project
dart pub get

# 2. 一键生成桥接项目脚手架（配置 + CMake + hook + 示例 API）
dcb_gen init

# 3. 直接运行 —— Native Assets hook 自动构建 C++
dart run
```

`dcb_gen init` 会生成完整可运行的示例（头文件 + 实现 + CMake + hook），
无需手动编写任何 C++ 代码即可 `dart run`。之后再替换示例 API 为你自己的函数。

## 命令

| 命令 | 说明 |
|------|------|
| `dcb_gen init` | 生成桥接项目脚手架（配置、CMake、hook、示例 API + 实现） |
| `dcb_gen generate <config.yaml>` | 运行完整 codegen 流程（解析 C++ → 生成 Dart + C++ wire） |
| `dcb_gen bootstrap` | 下载并校验固定版本的 Python + libclang 工具链 |
| `dcb_gen doctor` | 检查环境状态（Dart SDK、工具链缓存、CMake） |

### `dcb_gen init` 详情

生成以下文件（已存在的文件自动跳过）：

```
dart_cpp_bridge.yaml        # codegen 配置
native/CMakeLists.txt       # CMake 构建（自动解析 dart_cpp_bridge 包路径）
native/api/bridge_api.h     # 示例 C++ 头文件（带 BRIDGE_* 注解）
native/api_impl/bridge_api.cpp  # 示例实现（可直接运行）
hook/build.dart             # Native Assets 构建 hook
```

选项：`--name <lib>`（省略时自动从 `pubspec.yaml` 读取）。

## 选项

| 选项 | 说明 |
|------|------|
| `--force` | 强制重新下载，即使缓存的工具链有效 |
| `--quiet` | 静默模式，仅输出错误 |
| `--version` | 打印版本号并退出 |
| `--help` | 显示帮助信息 |

## 配置文件

`dart_cpp_bridge.yaml` 告诉工具解析哪些头文件、输出到哪里。示例：

```yaml
dart_package: my_app
cpp_root: native/

scan:
  - native/api/

include_paths:
  - native
  - native/api

dart_output: lib/src/native_gen/
cpp_wire_output: native/generated/

# 可选：clang-format 候选路径（从上到下尝试，最后回退到 PATH）。
clang_format:
  - C:\Program Files\LLVM\bin

std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
```

| 字段 | 说明 |
|------|------|
| `dart_package` | Dart 包名（必须与 `pubspec.yaml` 的 name 一致） |
| `cpp_root` | C++ 源码根目录 |
| `scan` | 扫描注解头文件的目录 |
| `include_paths` | 项目相对 include 路径（传给 clang） |
| `dart_output` | 生成的 Dart 代码输出目录 |
| `cpp_wire_output` | 生成的 C++ wire dispatch 输出目录 |
| `clang_format` | 可选，clang-format 路径列表（目录或可执行文件） |
| `std` | C++ 标准（默认 `c++20`） |
| `defines` | 传给 clang 的预处理宏 |
| `dart_code` | 可选，注入到数据类中的自定义 Dart 代码 |

可通过 `dart_code` 向生成的数据类注入自定义代码（如自定义 `toString()`），
存在时替换自动生成的 `toString()`：

```yaml
dart_code:
  Rect: |
    @override
    String toString() => 'Rect[$topLeft -> $bottomRight]';
```

## 工作原理

1. **Bootstrap**：下载 [python-build-standalone](https://github.com/astral-sh/python-build-standalone)
   和 [libclang-ng](https://pypi.org/project/libclang-ng/) wheel 包，校验
   SHA-256，缓存到平台特定目录：
   - Windows: `%LOCALAPPDATA%\dart_cpp_bridge\toolchain`
   - macOS: `~/Library/Caches/dart_cpp_bridge/toolchain`
   - Linux: `~/.cache/dart_cpp_bridge/toolchain`

2. **解析**：使用 libclang（通过 Python）解析 C++ 头文件，提取带有
   `BRIDGE_SYNC`、`BRIDGE_ASYNC`、`BRIDGE_NORMAL`、`BRIDGE_OPAQUE`、
   `BRIDGE_DATA_CLASS` 标记的函数和类。

3. **生成**：输出：
   - **Dart API 层**（`api/*.dart`）—— 面向用户的薄转发函数/类
   - **Dart Impl 层**（`dcb_generated.dart`）—— 编解码、wire 调度、生命周期
   - **C++ wire dispatch**（`wire_dispatch.hpp/.cpp`）—— 帧路由、调度
   - **IR**（`ir.json`）—— 中间表示，用于调试

## C++ 注解标记

```cpp
#include <dart_cpp_bridge/annotate.h>

BRIDGE_SYNC int32_t add(int32_t a, int32_t b);
BRIDGE_ASYNC Lazy<std::string> fetch_data(std::string url);
BRIDGE_NORMAL void tick(StreamSink<int32_t> sink);

BRIDGE_OPAQUE class Counter {
public:
  BRIDGE_SYNC static Counter* new_with_initial_value(int32_t v);
  BRIDGE_ASYNC int32_t value();
};
```

## 环境变量

| 变量 | 说明 |
|------|------|
| `DCB_CODEGEN_CACHE` | 覆盖工具链缓存目录 |

## 许可证

MIT
