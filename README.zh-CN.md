# dart_cpp_bridge

[English](README.md) | **中文**

面向 **Dart / Flutter** 的 **C++20** 互操作桥：用协程与事件循环把现有 C/C++ 代码方便地接到 Dart 侧，体验尽量对齐 [Flutter Rust Bridge (FRB)](https://cjycode.com/flutter_rust_bridge/)。

仓库：<https://github.com/deretame/dart_cpp_bridge>

> **状态**：独立实验仓库。Phase 1 手写运行时已基本完成；Phase 2 codegen 已能对 fixture 生成 **SYNC/ASYNC/NORMAL**（见 `examples/codegen_demo`）。**Native Assets / 富类型 / 产品化模板**尚未就绪，尚不适合作为生产依赖直接替换 FRB。

---

## 为什么要做这个项目？

生态里：

- **Rust** 有成熟的 [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/)，从业务函数到 Dart API 链路清晰；
- **C / C++** 仍有大量存量代码（多媒体、网络、游戏、嵌入式、历史业务库……），却缺少一套**同样顺手**的 Dart 接入方案。

常见路径要么是手写 FFI + 自管线程与回调，要么是 JNI / 平台通道碎片化，异步与流式调用容易写成「回调地狱」或堵死事件循环。

**本项目的目标**是补上这块空白：

- 给 C++ 业务一个**清晰的接入面**（sync / async / stream / 反向 Dart 闭包）；
- 用 **C++20 协程**编写异步逻辑，语义上贴近 Dart 的 `async` / `await` / `Stream`；
- 运行时模型参考 FRB（进程级 Runtime、Isolate 级 Session、长期 port + `request_id`），降低心智切换成本。

---

## 设计理念

| 原则 | 说明 |
|------|------|
| 对齐 FRB 体验 | 通道划分、生命周期、DartFn 反向调用等概念与 FRB 同构，便于两边对照 |
| 协程优先 | 业务侧写 `async_simple::coro::Lazy<T>`，在 io 上 `co_await`，而不是到处 `std::future::get` |
| 不替用户兜底错误用法 | 例如在 io 线程上 `callSync` 会堵调度器——文档写清，由调用方负责 |
| 渐进落地 | Phase 1 手写 demo 验证模型 → Phase 2 codegen → Phase 3 Native Assets / 产品化 |

业务代码**不**需要返回「桥专用 Future 包装」；正常写同步函数或 `Lazy`，由桥负责编解码与调度。

---

## 技术栈

| 层级 | 选型 | 作用 |
|------|------|------|
| 语言 | **C++20**（最低要求） | 协程、概念等；请使用支持 C++20 的编译器（MSVC 较新版本 / GCC / Clang） |
| 事件循环 | **[Asio](https://think-async.com/Asio/)**（standalone） | 单线程 `io_context`：定时、post、与外部完成对接 |
| 协程与管道 | **[async-simple](https://github.com/alibaba/async-simple)** | `Lazy` 协程、`Executor` 调度；配合本仓库 `co::oneshot` / mpsc 等 channel |
| Dart 侧 | Dart 3 + `package:ffi` | Isolate、ReceivePort、Completer / Stream |
| 参考实现 | Flutter Rust Bridge | 架构与 API 形态对照，而非代码依赖 |

运行时关系（简化）：

```text
┌─────────────────────────────────────────────────────────┐
│  Dart Isolate(s)                                        │
│    Session（每 Isolate 一个 reply port）                  │
│    Future / Stream / DartFn 回调                        │
└──────────────────────────▲──────────────────────────────┘
                           │ FFI + 二进制 frame
┌──────────────────────────┴──────────────────────────────┐
│  Runtime（进程唯一）                                      │
│    asio::io_context（单线程）+ AsioExecutor              │
│    asio::thread_pool（normal / 阻塞型工作）               │
│    wire：sync / async Lazy / stream / DartFn            │
└─────────────────────────────────────────────────────────┘
```

**DartFn（C++ → Dart 闭包）async 路径**：在 io 上 `co_await` oneshot 真挂起，**不堵 io 线程、不占 pool 线程**；sync 路径阻塞当前线程（在 io 上调用则自负）。

---

## 当前能力（Phase 1）

| 能力 | 状态 |
|------|------|
| 单线程 asio + thread_pool | ✅ |
| Runtime 进程唯一 / Session 每 Isolate | ✅ |
| sync / async（Lazy）/ normal / stream | ✅ |
| DartFn 反向调用（参数式闭包，类 FRB） | ✅（async = io 真挂起） |
| NativeFinalizer 自动关 session | ✅ |
| Dart 测试（含多 Isolate） | ✅ |
| C++ smoke（oneshot / DartFn e2e） | ✅ |
| Codegen（扫头 + 标记 → wire / Dart） | ⏳ SYNC/ASYNC/NORMAL + Dart 三层 API；见 `examples/codegen_demo` |
| Native Assets hook 产品化 | ⏳ 未开始 |

### Demo API（手写）

| C++ / 概念 | Dart | 通道 |
|------------|------|------|
| `bridgeVersion` | `int bridgeVersion()` | sync |
| `add(a, b)` | `Future<int> add(a, b)` | async（Lazy） |
| `sleepTest` | `Future<String> sleepTest()` | normal（pool） |
| `ticks(count, intervalMs)` | `Stream<int> ticks(...)` | stream |
| `callDartHello(cb)` | `Future<String> callDartHello(cb)` | DartFn **async**（io `co_await`） |
| `callDartHelloSync(cb)` | `Future<String> callDartHelloSync(cb)` | DartFn **sync**（堵当前线程） |

---

## 快速开始

### 环境

- CMake ≥ 3.20  
- **C++20** 编译器  
- Git（FetchContent 拉取 Asio / async-simple）  
- **Dart SDK ≥ 3.10**（`dart/pubspec.yaml`：`sdk: ^3.10.0`）  
  - Build hooks / Native Assets：≥ 3.10（当前稳定版 Flutter 自带 Dart 3.12.x 即可）  
  - Link hooks + recorded-usage 摇树：≥ 3.13（稳定版普及后再考虑抬下限）

### 构建 C++

```bash
# 1) 拉取 Dart API DL 头文件
cmake -P cmake/fetch_dart_api.cmake

# 2) 配置并编译
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 3) 原生 smoke
./build/dcb_smoke                 # Linux / macOS（路径因生成器而异）
build/Release/dcb_smoke.exe       # Windows 多配置生成器
```

Windows（PowerShell）：

```powershell
cmake -P cmake/fetch_dart_api.cmake
cmake -S . -B build
cmake --build build --config Release
.\build\Release\dcb_smoke.exe
```

### Dart 侧

```powershell
cd dart
dart pub get
```

```dart
import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

Future<void> main() async {
  final b = await DartCppBridge.init(
    libraryPath: r'..\build\Release\dart_cpp_bridge.dll', // 按平台改路径
  );

  print(b.bridgeVersion());
  print(await b.add(40, 2));
  print(await b.sleepTest());

  // DartFn：C++ 在 io 上 await，Dart 侧跑闭包
  print(await b.callDartHello((name) => 'Hello, $name!'));
  print(await b.callDartHello((name) async {
    await Future<void>.delayed(const Duration(milliseconds: 10));
    return 'Hello async, $name!';
  }));

  await for (final n in b.ticks(count: 3, intervalMs: 0)) {
    print('tick $n');
  }

  // 进程退出前（仅主 isolate）
  b.shutdown();
}
```

### 测试

```powershell
# C++ smoke（无 Dart VM，模拟 post / DartFn reply）
.\build\Release\dcb_smoke.exe

# Dart FFI 全量测试（需先编出动态库）
cd dart
dart test
```

默认从仓库 `build/Release/dart_cpp_bridge.dll`（或对应平台 so/dylib）加载；也可：

```powershell
$env:DCB_LIBRARY_PATH = "D:\path\to\dart_cpp_bridge.dll"
dart test
```

---

## 生命周期（对齐 FRB 思路）

| 动作 | 谁调用 | 说明 |
|------|--------|------|
| `DartCppBridge.init` | 每个要用桥的 Isolate | 打开本 isolate 的 Session + reply port |
| `dispose` | 可选 | 立即关 session；**多数情况可不调** |
| `NativeFinalizer` | 自动 | 对象不可达 / isolate 结束时 `session_close` |
| `shutdown` | **仅主 isolate 退出时** | 停进程级 Runtime；worker 勿调 |

模型：**Runtime 进程唯一；Session 每 Isolate 一个**——后台 isolate 也可独立 async / stream。

---

## 仓库结构

```text
docs/
  frb_and_cpp_bridge_design.md   # 设计全文
  progress.md                    # 实现进度
  known_issues.md                # 技术债与已解问题
include/dart_cpp_bridge/         # 公共 C++ 头（runtime / session / channel / DartFn …）
src/                             # runtime · wire · ffi_entry
third_party/dart_api/            # Dart API DL
dart/                            # Dart 包 + test
examples/phase1_demo/            # C++ smoke
cmake/                           # 工具脚本
codegen/                         # 固定工具链 + parse/generate
examples/codegen_demo/           # codegen fixture（yaml + 生成物 + 测试）
hook/                            # Native Assets 预留
```

---

## 文档

| 文档 | 内容 |
|------|------|
| [docs/frb_and_cpp_bridge_design.md](docs/frb_and_cpp_bridge_design.md) | 设计决策、通道模型、与 FRB 对照 |
| [docs/progress.md](docs/progress.md) | 实现进度与已落地清单 |
| [docs/known_issues.md](docs/known_issues.md) | 已知问题 / 已解决问题（含 DartFn oneshot） |

设计与进度文档目前以**中文**为主。

---

## 路线图（摘要）

1. **Phase 1**（当前）：手写骨架验证 Runtime / Session / 四通道 / DartFn  
2. **Phase 2**：基础 SYNC/ASYNC/NORMAL 已通 → 扩展 struct/Stream/DartFn 与模板  
   文档：[`codegen/README.md`](codegen/README.md)、[`examples/codegen_demo`](examples/codegen_demo/)
3. **Phase 3**：Native Assets hook、CMake export、示例工程产品化  
4. **Phase 4**：业务接入（不默认替换任何已有 FRB 生产桥）

---

## 非目标（当前）

- 不保证 ABI / API 稳定  
- 不提供「在 io 上 sync 阻塞也帮你自动离载」的隐藏魔法  
- 不把 C++ 标准库或业务二进制打包进 pub 包（后续 hook 负责编链接）  
- 不替代平台通道去画 UI；只做 **Dart ↔ 原生逻辑** 的桥

---

## 致谢与参考

- [Flutter Rust Bridge](https://github.com/fzyzcjy/flutter_rust_bridge) — 架构与产品形态参考  
- [Asio](https://think-async.com/Asio/) — 事件循环与异步 I/O  
- [async-simple](https://github.com/alibaba/async-simple) — C++20 协程运行时  
- Dart / Flutter 团队 — FFI、Isolate、NativeFinalizer 等能力  

---

## 许可

本项目采用 **MIT License** — 见 [LICENSE](LICENSE)。

Asio、async-simple、Dart SDK 头文件等第三方组件仍遵循各自许可证。

---

## 参与

问题与设计讨论请先看 `docs/`。欢迎针对 Phase 1 行为、测试覆盖与文档清晰度提 issue / PR：  
<https://github.com/deretame/dart_cpp_bridge/issues>
