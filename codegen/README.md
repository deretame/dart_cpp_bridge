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

```bash
cd codegen
dart pub get
dart run bin/codegen.dart                          # 默认 smoke_toolchain
dart run bin/codegen.dart --force                  # 重建 env
dart run bin/codegen.dart scripts/run_codegen.py /path/to/dart_cpp_bridge.yaml
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
| `BRIDGE_DATA_CLASS` / `DCB_DATA_CLASS` | 类标记：纯数据类（只有字段，无方法） |
| `BRIDGE_OPAQUE` / `DCB_OPAQUE` | 类标记：opaque 类（只生成方法，忽略公开字段） |

无标记 → 忽略（可与导出 API 同文件）。

### 类标记说明

| 宏 | 语义 | 校验 |
|----|------|------|
| `BRIDGE_DATA_CLASS` | 纯数据类：字段序列化/反序列化，不生成方法 | 不能有 `BRIDGE_SYNC/ASYNC/NORMAL` 方法、不能继承、不能有虚函数 |
| `BRIDGE_OPAQUE` | Opaque 类：对齐 FRB `RustAutoOpaque`，只生成标注的方法 | 不能继承、不能有虚函数；公开字段被忽略 |
| `BRIDGE_EXPORT`（旧） | 自动检测：有导出方法→opaque，否则→data_class | 同上两者 |

**Opaque 类字段访问**：若需读写公开字段，请手写 `BRIDGE_SYNC` getter/setter 方法。

```cpp
// 示例：opaque 类 + 手写字段访问
struct BRIDGE_OPAQUE Config {
  int timeout_ms;  // 公开字段，codegen 不生成访问器
  std::string name;

  BRIDGE_CONSTRUCTOR Config(int timeout_ms, std::string name);
  BRIDGE_SYNC int getTimeoutMs() const { return timeout_ms; }
  BRIDGE_SYNC void setTimeoutMs(int v) { timeout_ms = v; }
};
```

**实现注意：** codegen 路径宏展开为 `__attribute__((annotate("bridge::*")))`。  
未知的 `[[bridge::*]]` 会被 clang 丢掉，**AST 不可见**，不能用于过滤。  
业务编译不定义 `BRIDGE_CODEGEN`，宏为空。

当前类型支持（v1）：`int32_t` / `std::string` / `Lazy<上述>` / `enum class T : int32_t` / `std::optional<T>`；struct / Stream 后续扩展。

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

| 文件 | 作用 |
|------|------|
| `bin/codegen.dart` | CLI 入口（bootstrap + 跑 Python） |
| `lib/src/platform.dart` | OS/架构检测、cache 路径 |
| `lib/src/lock_file.dart` | versions.lock 解析 |
| `lib/src/bootstrap.dart` | 下载/校验/解压/冒烟 |
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
| 枚举（enum class）生成 | ✅ |
| Native Assets hook 集成 | ❌（Phase 3；hook 仍不跑 codegen） |
