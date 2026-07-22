# Codegen

手动触发的 API 代码生成：固定版 Python + libclang-ng 解析 C++ 头 → IR → C++ wire + Dart 绑定。  
**不**由 Native Assets hook 调用。设计全文见 [docs/frb_and_cpp_bridge_design.md](../docs/frb_and_cpp_bridge_design.md) §6。

端到端 fixture：[examples/codegen_demo](../examples/codegen_demo/)。

---

## 1. 工具链（`versions.lock`）

| 组件 | 版本 | 来源 |
|------|------|------|
| **Python** | **3.13.13** | python-build-standalone `20260414` / `install_only_stripped` |
| **libclang** | **libclang-ng 22.1.4.2** | PyPI wheel（含原生 libclang） |

包内只提交 lock + 脚本；运行时下载到**用户级 cache**。

### Cache 布局

| OS | 默认根目录 |
|----|------------|
| Windows | `%LOCALAPPDATA%\dart_cpp_bridge\toolchain` |
| macOS | `~/Library/Caches/dart_cpp_bridge/toolchain` |
| Linux | `${XDG_CACHE_HOME:-~/.cache}/dart_cpp_bridge/toolchain` |

覆盖：环境变量 `DCB_CODEGEN_CACHE`。

```text
<cache>/
  downloads/<sha256>.tar.gz|.whl   # blob 去重
  envs/<platform>-<fp16>/          # 每套 lock 指纹一个 env
    python/
    READY.json
  LAST_ENV.json
  tmp/
```

- 同一 lock + 平台 → 共用 env  
- 不同 lock → 不同文件夹，互不覆盖  

### 入口

```powershell
cd codegen
.\codegen.ps1                                              # 默认 smoke_toolchain
.\codegen.ps1 -Force                                       # 重建 env
.\codegen.ps1 -Script scripts/run_codegen.py -- <yaml路径>
```

```bash
cd codegen
chmod +x codegen.sh bootstrap.sh
./codegen.sh
./codegen.sh scripts/run_codegen.py /path/to/dart_cpp_bridge.yaml
```

---

## 2. 用户工程配置（`dart_cpp_bridge.yaml`）

放在**用户工程根**（或 `--config` 指定）。示例见 `examples/codegen_demo/dart_cpp_bridge.yaml`。

| 字段 | 含义 |
|------|------|
| `cpp_root` | C++ 根（相对 yaml） |
| `scan` | 扫描目录列表；只处理其下 `.h` / `.hpp` |
| `include_paths` | libclang `-I` |
| `dart_output` | Dart 生成目录 |
| `cpp_wire_output` | C++ wire 生成目录 |
| `std` / `defines` | 默认 `c++20`，codegen 需 `BRIDGE_CODEGEN` |
| `dart_impl_class` 等 | 可选：类名/文件名（见下） |

### 扫描与标记

1. 枚举 `scan` 下所有头文件  
2. libclang 解析（`-DBRIDGE_CODEGEN`）  
3. **仅**带生成标记的声明进入 IR  

| 标记（宏） | 通道 |
|------------|------|
| `BRIDGE_SYNC` / `DCB_SYNC` | Dart 同步 |
| `BRIDGE_ASYNC` / `DCB_ASYNC` | Dart `Future`，C++ `Lazy` + asio |
| `BRIDGE_NORMAL` / `DCB_NORMAL` | Dart `Future`，blocking 线程池 |
| 参数含 `StreamSink<T>` | Dart `Stream`（生成器尚未全实现） |

无标记 → 忽略（可与导出 API 同文件）。

**实现注意：** codegen 路径宏展开为 `__attribute__((annotate("bridge::*")))`。  
未知的 `[[bridge::*]]` 会被 clang 丢掉，**AST 不可见**，不能用于过滤。  
业务编译不定义 `BRIDGE_CODEGEN`，宏为空。

当前类型支持（v1）：`int32_t` / `std::string` / `Lazy<上述>`；struct / Stream 后续扩展。

---

## 3. 生成物

### C++

| 文件 | 说明 |
|------|------|
| `wire_dispatch.hpp` / `.cpp` | `dcb::demo::dispatch_request` / `dispatch_sync`（供 `ffi_entry` 链接） |
| `ir.json` | 中间表示，调试用 |

业务实现仍由用户手写（如 `api_impl/*.cpp`），wire 只负责编解码与调度。

### Dart（三层，类 FRB）

| 文件（默认名） | 类/符号 | 用途 |
|----------------|---------|------|
| `api.g.dart` | `BridgeApiImpl` | method id、编解码、调 `DartCppBridge.invoke*` |
| `api.dart` | `BridgeApi.instance` | 单例：`init` / `dispose` / 转发 |
| `api_fn.dart` | 顶层函数 | `initBridge()`、`add()`… 直接调用 |

```text
业务代码
  → api_fn.dart（顶层函数）          // 推荐 call site
  → api.dart（BridgeApi.instance）
  → api.g.dart（BridgeApiImpl）
  → DartCppBridge FFI
```

可选 yaml 键：

```yaml
dart_impl_class: BridgeApiImpl
dart_api_class: BridgeApi
dart_impl_file: api.g.dart
dart_api_file: api.dart
dart_fn_file: api_fn.dart
```

业务侧推荐：

```dart
import 'package:your_app/src/native_gen/api_fn.dart';

await initBridge(libraryPath: '...');
final v = bridgeVersion();
final s = await add(1, 2);
shutdownBridge(); // 仅进程退出
```

也可用单例：`BridgeApi.instance.init(...)` / `.add(...)`。

---

## 4. 脚本一览

| 脚本 | 作用 |
|------|------|
| `bootstrap.ps1` / `.sh` | 下载校验工具链 |
| `codegen.ps1` / `.sh` | 入口（bootstrap + 跑 Python） |
| `scripts/smoke_toolchain.py` | 工具链冒烟 |
| `scripts/config_util.py` | 读简易 yaml |
| `scripts/parse_api.py` | 扫头 → IR |
| `scripts/generate.py` | IR → C++/Dart |
| `scripts/run_codegen.py` | parse + generate |

---

## 5. 状态

| 项 | 状态 |
|----|------|
| 固定 Python / libclang-ng + 用户 cache | ✅ |
| yaml + scan + 标记过滤 | ✅ |
| SYNC / ASYNC / NORMAL 生成 | ✅ `examples/codegen_demo` |
| Dart 三层（impl / 单例 / 顶层函数） | ✅ |
| struct / Stream / DartFn 生成 | ❌ |
| Native Assets hook 集成 | ❌（Phase 3；hook 仍不跑 codegen） |
