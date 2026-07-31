# codegen_demo — 项目模板

本目录既是 Phase 2 的端到端测试 fixture，也是**可复制的用户项目模板**。

如果你想在自己的 C++ 库上接入 `dart_cpp_bridge`，从这里开始：复制本目录，改配置和头文件，跑 codegen，编 DLL，Dart 侧即可调用。

更完整的 codegen 说明见 [dcb_gen_tool/README.md](../../dcb_gen_tool/README.md)。

---

## Quick Start（复制本模板后）

```text
1. 复制本目录到你的工程
2. 修改 dart_cpp_bridge.yaml（scan 路径、include_paths、输出路径）
3. 在 native/api/ 下写带 BRIDGE_* 标记的头文件
4. 在 native/api_impl/ 下写 C++ 实现
5. 跑 codegen（生成 wire + Dart 绑定）
6. CMake 编译动态库
7. Dart 侧 import 生成的 API，调用
```

---

## 目录结构

```text
examples/codegen_demo/
  dart_cpp_bridge.yaml       # codegen 配置（你需要改这个）
  native/
    api/                     # 带 BRIDGE_* 的声明（scan 输入，你改这里）
      bridge_api.h           # 顶层函数 API
      counter.h              # Opaque 类 API
      point_rect.h           # 数据类 API
    api_impl/                # 用户手写实现（你改这里）
      bridge_api.cpp
      counter.cpp
    generated/               # codegen 输出（勿手改）
      wire_dispatch.hpp/cpp  # C++ dispatch
      ir.json                # 中间表示
  lib/
    main.dart                # Flutter 应用壳（真机上跑桥接测试）
    codegen_demo.dart        # export 生成 API
    src/native_gen/          # codegen 输出的 Dart 三层（勿手改）
      api.g.dart             # BridgeApiImpl（底层）
      api.dart               # BridgeApi.instance（单例）
      api_fn.dart            # 顶层函数（推荐调用）
  test/api_test.dart         # 端到端测试（flutter test）
  integration_test/          # Android 真机集成测试
  android/                   # Flutter Android 宿主应用
  CMakeLists.txt             # 编动态库
```

---

## 1. 配置 `dart_cpp_bridge.yaml`

```yaml
cpp_root: native/            # C++ 工程根

scan:                        # 只扫这些目录下的 .h/.hpp
  - native/api/

include_paths:               # libclang 解析时的 -I 路径
  - native
  - native/api
  - ../../include            # dart_cpp_bridge 公共头
  - ../../dcb_gen_tool/stubs      # async_simple stub

dart_output: lib/src/native_gen/   # Dart 生成物
cpp_wire_output: native/generated/ # C++ wire 生成物

std: c++20
defines:
  - BRIDGE_CODEGEN
  - DART_CPP_BRIDGE_CODEGEN
```

**接入你的项目时**：把 `include_paths` 改为你的 dart_cpp_bridge 仓库路径（或 FetchContent 后的路径）。

---

## 2. 写 API 头文件

在 `native/api/` 下新建 `.h` 文件，用 `BRIDGE_*` 标记导出：

```cpp
#pragma once
#include "dart_cpp_bridge/annotate.h"
#include <async_simple/coro/Lazy.h>
#include <cstdint>
#include <string>

namespace my_api {

// sync → Dart: int bridgeVersion()
BRIDGE_SYNC
std::int32_t bridge_version();

// async → Dart: Future<int> add(int a, int b)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);

// normal（线程池）→ Dart: Future<String> sleepGreeting(String name)
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

}  // namespace my_api
```

支持的类型见 [docs/codegen_type_mapping.md](../../docs/codegen_type_mapping.md)。

---

## 3. 跑 Codegen

```bash
cd <repo>/codegen
dart pub get
dart run bin/codegen.dart scripts/run_codegen.py ../examples/codegen_demo/dart_cpp_bridge.yaml
```

首次会下载固定 Python + libclang-ng 到用户 cache（见 codegen README）。

生成物：

| 路径 | 内容 |
|------|------|
| `native/generated/wire_dispatch.*` | C++ dispatch |
| `native/generated/ir.json` | IR |
| `lib/src/native_gen/api.g.dart` | 底层 impl |
| `lib/src/native_gen/api.dart` | 单例 facade |
| `lib/src/native_gen/api_fn.dart` | 顶层函数 |

**类型错误**：如果头文件中使用了白名单外的类型，codegen 会报出清晰的错误（含文件名:行号 + 提示），不会生成错误代码。

---

## 4. 编译原生库

**先**在仓库根编过主工程（或已有 `build/_deps`），demo CMake 会复用 asio / async-simple，避免二次 git clone。

```powershell
# 仓库根（若尚无 _deps）
cmake -S dart/native -B dart/native/build
cmake --build dart/native/build --config Release

# demo
cd examples\codegen_demo
cmake -S native -B build
cmake --build build --config Release
```

产出：`build/Release/dcb_codegen_demo.dll`（或 `.so` / `.dylib`）。

该 DLL 含：runtime + `ffi_entry` + **生成的 wire** + 用户 `api_impl`（**不含**主工程手写 `demo_api.cpp`）。

### 外部项目接入（FetchContent）

如果你的项目不在 dart_cpp_bridge 仓库内，可通过 CMake FetchContent 拉取：

```cmake
include(FetchContent)
FetchContent_Declare(
  dart_cpp_bridge
  GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
  GIT_TAG        main   # 或钉死 tag/commit
)
FetchContent_MakeAvailable(dart_cpp_bridge)

# dart_cpp_bridge 目标已 PUBLIC 暴露 asio + async-simple
target_link_libraries(my_bridge PRIVATE dart_cpp_bridge)
```

> 注：当前 FetchContent 接入尚未产品化（Phase 3），目前推荐在 monorepo 内或手动指定 `DCB_ROOT`。

---

## 5. 测试

本包是 Flutter 包（含 Android 宿主应用），用 `flutter test` 而非 `dart test`：

```powershell
cd examples\codegen_demo
flutter pub get
flutter test
```

### Android 集成测试

同一套桥接测试也可以通过 `integration_test/` 在真机/模拟器上运行：

```bash
# 需要连接设备或运行中的模拟器（x86_64/arm64）。
# Native Assets hook 会用 NDK 交叉编译原生库。
flutter test integration_test
```

覆盖：

| 标记 | C++ | Dart 顶层调用 | 期望 |
|------|-----|---------------|------|
| `BRIDGE_SYNC` | `bridge_version` | `bridgeVersion()` | `42` |
| `BRIDGE_ASYNC` | `add` | `await add(2, 3)` | `5` |
| `BRIDGE_NORMAL` | `sleep_greeting` | `await sleepGreeting('Ada')` | `hello, Ada` |
| `BRIDGE_DATA_CLASS` | `distance(Point, Point)` | `await distance(...)` | `double` |
| `BRIDGE_OPAQUE` | `Counter` | `Counter.withInitialValue(...)` | 实例方法 |

无标记的 `internal_helper` **不会**出现在生成物中。

---

## 6. 业务调用方式

推荐（顶层函数）：

```dart
import 'package:codegen_demo/codegen_demo.dart';

await initBridge(libraryPath: r'...\dcb_codegen_demo.dll');
print(bridgeVersion());
print(await add(1, 2));
print(await sleepGreeting('world'));
shutdownBridge(); // 仅进程退出时
```

单例等价：

```dart
await BridgeApi.instance.init(libraryPath: '...');
BridgeApi.instance.bridgeVersion();
```

---

## 7. 自定义清单

复制本模板后，通常需要改的地方：

| 文件 | 改什么 |
|------|--------|
| `dart_cpp_bridge.yaml` | `scan` 路径、`include_paths`、输出路径 |
| `native/api/*.h` | 你的 API 声明 |
| `native/api_impl/*.cpp` | 你的业务实现 |
| `CMakeLists.txt` | target 名、源文件列表、`DCB_ROOT` 路径 |
| `pubspec.yaml` | 包名、依赖 |
| `lib/codegen_demo.dart` | export 路径 |

**不需要改**的：
- `native/generated/` — codegen 自动覆盖
- `lib/src/native_gen/` — codegen 自动覆盖
- `include/dart_cpp_bridge/` — 桥公共头，直接引用

---

## 8. 头文件约定

```cpp
// native/api/bridge_api.h
BRIDGE_SYNC   std::int32_t bridge_version();
BRIDGE_ASYNC  async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);
BRIDGE_NORMAL std::string sleep_greeting(std::string name);
```

实现写在 `native/api_impl/`，改实现**不必**重跑 codegen；改签名/新增导出 API 后才需要。
