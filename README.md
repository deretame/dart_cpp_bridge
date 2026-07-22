# dart_cpp_bridge

类 [Flutter Rust Bridge](https://cjycode.com/flutter_rust_bridge/) 的 **Dart ↔ C++20** 桥（独立实验仓库）。

文档：

- 设计全文：[`docs/frb_and_cpp_bridge_design.md`](docs/frb_and_cpp_bridge_design.md)
- **实现进度**：[`docs/progress.md`](docs/progress.md)

## 当前状态（Phase 1）

手写骨架已基本完成，**无 codegen**。摘要：

| 能力 | 状态 |
|------|------|
| 单线程 asio + thread_pool | ✅ |
| Session 每 Isolate + Runtime 进程共享 | ✅ |
| sync / async / normal / stream | ✅ |
| NativeFinalizer 自动关 session | ✅ |
| Dart 测试（含多 isolate async） | ✅ |
| Codegen / Native Assets hook | ⏳ |

Demo API：

- `bridgeVersion()` sync → `int`
- `add(a,b)` async (`Lazy`) → `Future<int>`
- `sleepTest()` normal（`spawn_blocking`）→ `Future<String>`
- `ticks(count, intervalMs)` stream → `Stream<int>`

## 目录

```text
docs/                      # 设计 + 进度
include/dart_cpp_bridge/   # 公共头
src/                       # runtime / session / wire / ffi
third_party/dart_api/      # Dart API DL
dart/                      # Dart 包 + test
examples/phase1_demo/      # C++ smoke
cmake/                     # 工具脚本
codegen/                   # Phase 2 预留
```

## 构建（C++）

依赖：CMake ≥ 3.20、C++20 编译器、Git（FetchContent 拉 asio / async-simple）。

```bash
# 1) 拉取 Dart API DL 头文件
cmake -P cmake/fetch_dart_api.cmake

# 2) 配置并编译
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 3) 原生 smoke（无 Dart isolate，用回调打印 post）
./build/dcb_smoke          # Linux/macOS
build/Release/dcb_smoke.exe  # Windows 多配置生成器
```

Windows 示例（PowerShell）：

```powershell
cmake -P cmake/fetch_dart_api.cmake
cmake -S . -B build
cmake --build build --config Release
.\build\Release\dcb_smoke.exe
```

## Dart 侧

```powershell
cd dart
dart pub get
```

```dart
final b = await DartCppBridge.init(
  libraryPath: r'..\build\Release\dart_cpp_bridge.dll',
);
print(b.bridgeVersion());
print(await b.add(40, 2));
print(await b.sleepTest());
b.shutdown();
```

### 测试（sync / async FFI）

先编译出动态库，再在 `dart/` 下：

```powershell
cd dart
dart test
```

覆盖：sync / async / normal / stream、关订阅、dispose / shutdown、错误路径、坏帧、大 payload、
**多 Isolate 异步**（每 isolate 自己 `init` 开 session + reply port，runtime 进程共享）、纯 Dart codec。

模型：**Runtime 进程唯一；Session 每 Isolate 一个**（后台 isolate 也可 async/stream）。

生命周期（对齐 FRB 思路）：
- isolate 使用前 `init`
- **一般不必手动 `dispose`**：`NativeFinalizer` 在对象不可达 / isolate 结束时自动 `session_close`
- 可选 `dispose` 立即释放；`shutdown` 仅主 isolate 退出时停整个 runtime

默认从仓库 `build/Release/dart_cpp_bridge.dll` 加载；也可：

```powershell
$env:DCB_LIBRARY_PATH = "D:\path\to\dart_cpp_bridge.dll"
dart test
```

## 设计要点（已锁定）

- 业务写 `Lazy<T>` / 普通同步函数 / `StreamSink`；**不**返回桥 Future 包装。
- Codegen（后续）手动；Python + libclang **远端固定版**；hook 只编链接。
- 依赖库形态：CMake FetchContent，PUBLIC 暴露 asio + async-simple（后续完善 export）。

## 许可

实验项目；依赖 asio / async-simple / Dart SDK 头文件遵循各自许可证。
