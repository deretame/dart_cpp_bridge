# dcb_gen_tool

[dart_cpp_bridge](https://github.com/deretame/dart_cpp_bridge) 的代码生成 CLI 工具。

通过 libclang 解析带注解的 C++ 头文件，自动生成 Dart/C++ 桥接代码
（同步、异步、流、DartFn 反向调用、opaque 类）。

## 安装

```bash
dart pub global activate dcb_gen_tool
```

仅需 Dart SDK >= 3.5.0。无需安装 Python、LLVM 或 Rust ——
工具会自动下载经过 SHA-256 校验的固定版本 Python 工具链。

## 快速开始

```bash
# 1.（首次运行）下载工具链（约 100 MB，后续使用缓存）
dcb_gen bootstrap

# 2. 为项目生成桥接代码
cd my_project
dcb_gen generate dart_cpp_bridge.yaml
```

## 命令

| 命令 | 说明 |
|------|------|
| `dcb_gen generate <config.yaml>` | 运行完整 codegen 流程（解析 C++ → 生成 Dart + C++ wire） |
| `dcb_gen bootstrap` | 下载并校验固定版本的 Python + libclang 工具链 |
| `dcb_gen doctor` | 检查环境状态（Dart SDK、工具链缓存、CMake） |

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
library_name: demo
headers:
  - native/api/bridge_api.h
  - native/api/counter.h
include_paths:
  - native/api
dart_output: lib/src/native_gen
cpp_output: native/generated
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
