# codegen_demo

Phase 2 fixture：用 `dart_cpp_bridge.yaml` 扫描头文件，生成 **SYNC / ASYNC / NORMAL** 绑定，并跑 Dart 测试。

更完整的 codegen 说明见 [codegen/README.md](../../codegen/README.md)。

---

## 目录

```text
examples/codegen_demo/
  dart_cpp_bridge.yaml       # codegen 配置
  native/
    api/bridge_api.h         # 带 BRIDGE_* 的声明（scan 输入）
    api_impl/bridge_api.cpp  # 用户手写实现
    generated/               # codegen 输出：wire + ir.json
  lib/
    codegen_demo.dart        # export 生成 API
    src/native_gen/
      api.g.dart             # BridgeApiImpl（底层）
      api.dart               # BridgeApi.instance（单例）
      api_fn.dart            # 顶层函数（推荐调用）
  test/api_test.dart
  CMakeLists.txt             # 编 dcb_codegen_demo 动态库
```

---

## 1. Codegen

```powershell
cd <repo>\codegen
.\codegen.ps1 -Script scripts/run_codegen.py -- ..\examples\codegen_demo\dart_cpp_bridge.yaml
```

```bash
cd codegen
./codegen.sh scripts/run_codegen.py ../examples/codegen_demo/dart_cpp_bridge.yaml
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

---

## 2. 编译原生库

**先**在仓库根编过主工程（或已有 `build/_deps`），demo CMake 会复用 asio / async-simple，避免二次 git clone。

```powershell
# 仓库根（若尚无 _deps）
cmake -S . -B build
cmake --build build --config Release

# demo
cd examples\codegen_demo
cmake -S . -B build
cmake --build build --config Release
```

产出：`build/Release/dcb_codegen_demo.dll`（或 `.so` / `.dylib`）。

该 DLL 含：runtime + `ffi_entry` + **生成的 wire** + 用户 `api_impl`（**不含**主工程手写 `demo_api.cpp`）。

---

## 3. 测试

```powershell
cd examples\codegen_demo
dart pub get
dart test
```

覆盖：

| 标记 | C++ | Dart 顶层调用 | 期望 |
|------|-----|---------------|------|
| `BRIDGE_SYNC` | `bridge_version` | `bridgeVersion()` | `42` |
| `BRIDGE_ASYNC` | `add` | `await add(2, 3)` | `5` |
| `BRIDGE_NORMAL` | `sleep_greeting` | `await sleepGreeting('Ada')` | `hello, Ada` |

无标记的 `internal_helper` **不会**出现在生成物中。

---

## 4. 业务调用方式

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

## 5. 头文件约定（本 demo）

```cpp
// native/api/bridge_api.h
BRIDGE_SYNC   std::int32_t bridge_version();
BRIDGE_ASYNC  async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);
BRIDGE_NORMAL std::string sleep_greeting(std::string name);
```

实现写在 `native/api_impl/`，改实现**不必**重跑 codegen；改签名/新增导出 API 后才需要。
