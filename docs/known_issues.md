# 已知问题与技术债

> 记录实现过程中已确认的卡点，避免重复踩坑。  
> 更新日期：2026-07-27

---

## 1. 【已解决】DartFn / 外部事件可在 io 上真 `co_await`

### 1.1 目标形态（已达成）

```text
io 协程:  post DartFnCall → co_await oneshot（挂起，io 线程去干别的）
Dart Isolate: 执行回调 → dcb_dart_fn_reply → oneshot.send
io:       Executor::schedule(resume) → 继续业务
```

- **不会**在 io 上 `future.get()` / 忙等；
- 占用的是「协程挂起」，不是 pool 工作线程。

### 1.2 做法（对齐 FRB oneshot）

| 组件 | 角色 |
|------|------|
| `include/dart_cpp_bridge/channel.hpp` | `co::oneshot`：`coAwait(Executor*)` + `wake_waiter` → `ex->schedule(resume)` |
| `include/dart_cpp_bridge/asio_executor.hpp` | `AsioExecutor`：`schedule` = `asio::post(io, …)` |
| `Runtime::spawn_on_asio` | `lazy.via(executor).start(...)`，保证 Lazy 绑 executor |
| `Session::invoke_dart_fn_async` | `call_id → oneshot tx`；`co_await rx.recv()` |
| `kCallDartHello` | io 上 `co_await cb.callAsync(...)` |

`callSync` 仍为当前线程 `std::promise` + `get()`；堵 io 自负。

### 1.3 历史踩坑（保留备查）

| 尝试 | 现象 | 判断 |
|------|------|------|
| `async_simple::Promise` + FutureAwaiter | 30s 超时 | 与 executor 完成约定不匹配 |
| 仅 `asio::post(io, setValue)` | 仍不稳 | 未走 Lazy 的 `coAwait(Executor*)` |
| 裸 `coroutine_handle::resume` | AV | 不能当标准 coro 乱 resume |
| pool + `get()` | 能通但不干净 | 已替换为 oneshot |
| `spawn_on_asio` 里 coroutine lambda capture | gen=0 / AV | factory 在 `start()` 后销毁，capture 悬空；须 `shared_ptr` 保活到 Lazy 结束 |
| 成员函数 coroutine 读 `this->field` | 偶发错值 | `callAsync` 改为静态 Lazy，参数 by-value |

### 1.4 相关代码

- `include/dart_cpp_bridge/channel.hpp`
- `include/dart_cpp_bridge/asio_executor.hpp`
- `include/dart_cpp_bridge/dart_fn.hpp` — `callSync` / `callAsync`
- `src/runtime/runtime.cpp` — `invoke_dart_fn_async` / `complete_dart_fn`
- `src/wire/demo_api.cpp` — `kCallDartHello` / `kCallDartHelloSync`
- `examples/phase1_demo/smoke_main.cpp` — oneshot 跨线程唤醒 + io 不堵 测试

---

## 2. 【已绕过】normal / sleepTest 不宜依赖 `spawn_blocking`+FutureAwaiter

- **现象**：早期 wire 用 `co_await spawn_blocking(sleep)` 会挂起不返回。  
- **现状**：`sleepTest` / `ticks` 间隔等改为 `asio::post(thread_pool, ...)`，结果再 `post` 回业务（或直接 `try_post`）。  
- **后续**：若实现 `spawn_blocking` Lazy，应复用 oneshot / 同一套 `Executor::schedule` 唤醒。

---

## 3. 【已接受】设计演进：Session 每 Isolate 一个

- 设计原文偏单 session。  
- 为实现后台 Isolate 的 async/stream，改为 **Runtime 进程唯一 + Session 每 Isolate**。  
- 见 [progress.md](./progress.md) §3。

---

## 4. 【原则】不为用户兜底阻塞 io

- Sync DartFn：**不**自动离载到 pool。  
- 用户在 io 上 `callSync` → 调度器停转 → **用户问题**。  
- 文档与 API 命名需持续强调，避免误用。

---

## 5. 【环境依赖】Windows 上需要较新的 MSVC 运行时（MSVCP140.dll / VCRUNTIME140.dll）

### 5.1 现象

在 Windows 上运行 `dart test` 时，进程可能在 `NativeBindings` 初始化阶段崩溃：

```text
===== CRASH =====
ExceptionCode=-1073741819
...
pc 0x00007ffa... C:\Windows\SYSTEM32\MSVCP140.dll+0x18c34
...
[Optimized] new NativeBindings..#ffiClosure1+0x6d
[Unoptimized] DartCppBridge.init+...
```

C++ 的 `dcb_smoke.exe` 单独运行通常正常，因为 smoke 测试的 `.exe` 与 `MSVCP140.dll` 同目录。

### 5.2 原因

`dart_cpp_bridge` 默认使用 `/MD` 动态链接 MSVC 运行时。当前项目使用 Visual Studio 2026 / MSVC 14.51 编译，需要 **VC145** 版本的运行时 DLL（`MSVCP140.dll` 14.40+）。

如果系统中已安装的通用 `MSVCP140.dll` 是旧版本（例如 14.00.24215.1，对应 VS 2015），而 Dart 进程在启动时加载的是系统目录中的旧 DLL，则 C++ 侧构建出的 DLL 与该旧版本 ABI 不兼容，会在调用 C++ 标准库代码时崩溃。

### 5.3 临时解决

将 VS Redist 目录中的新版运行时 DLL 复制到 `dart.exe` 所在的目录（最高优先级），例如：

```powershell
# 以实际 VS 安装路径和版本为准
$src = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Redist\MSVC\14.51.36231\x64\Microsoft.VC145.CRT"
$dst = "$env:USERPROFILE\.puro\envs\default\flutter\bin\cache\dart-sdk\bin"
copy "$src\MSVCP140.dll" $dst
copy "$src\VCRUNTIME140.dll" $dst
copy "$src\VCRUNTIME140_1.dll" $dst
```

也可以将上述 DLL 复制到 `dart/` 目录（`dart test` 的当前目录），但优先级低于 `dart.exe` 所在目录。

### 5.4 彻底解决（推荐）

`dcb_shared` 目标已改为 **静态 CRT（/MT）**，不再依赖系统 MSVCP140.dll：

```cmake
# dart/native/CMakeLists.txt — dcb_shared 目标
set_property(TARGET dcb_shared PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

此方案使 hook 构建出的 `dart_cpp_bridge.dll` 完全自包含，无需用户安装任何 VC Redist。  
若仍使用 `/MD` 动态链接（如 base_demo 的旧 CMake 路径），则需安装最新 **Visual C++ Redistributable**（14.40+，VC145）。

### 5.5 相关检查

- 查看系统 DLL 版本：
  ```powershell
  (Get-ItemProperty C:\Windows\System32\MSVCP140.dll).VersionInfo.FileVersion
  ```
- 查看构建依赖的 DLL 版本：
  ```powershell
  dumpbin /DEPENDENTS build\Release\dart_cpp_bridge.dll
  ```

---

## 6. 一句话

**DartFn 反向调用：协议 + oneshot + AsioExecutor 已通；async 路径为 io 上真挂起，sync 路径仍阻塞当前线程。**

---

## 7. 【已解决】@Native 路径下 std::mutex::lock() 崩溃（MSVCP140.dll ABI 不兼容）

### 7.1 现象

Phase C 将 base 包绑定层迁移到 `@Native(assetId:)` 后，任何涉及 `std::mutex` 的 C++ 代码经 @Native 调用都会崩溃（访问违例 `0xC0000005`），而同一 DLL 经 `DynamicLibrary.open()` + `lookupFunction` 路径完全正常。

```text
===== CRASH =====
ExceptionCode=-1073741819
pc 0x00007ffa... C:\Windows\SYSTEM32\MSVCP140.dll+0x18c34  (mtx_do_lock)
```

### 7.2 调试路径

| 步骤 | 实验 | 结论 |
|------|------|------|
| 1 | `Runtime::start()` 委托为自由函数 | 仍崩 → 不是成员函数的问题 |
| 2 | 绕过 Runtime，手动 start/stop | 通过 → `dcb_shutdown` 是触发器 |
| 3 | 只调 `close_all()` 不 start | 仍崩 → `close_all` 内的 mutex |
| 4 | 全局 `std::mutex` + `lock()` | 仍崩 → 任何 mutex 都崩 |
| 5 | 堆上 `new std::mutex` | 仍崩 → 与存储位置无关 |
| 6 | `dumpbin /DEPENDENTS` + 版本检查 | MSVCP140.dll = 14.00（VS 2015） |

### 7.3 根因

系统 `MSVCP140.dll`（14.00.24215.1，VS 2015）中 `mtx_do_lock` 的内部实现与 MSVC 19.51（VS 2026）编译出的 `std::mutex` 对象布局不兼容。在 `DynamicLibrary.open` 路径下，Dart VM 的 DLL 搜索顺序可能先找到同目录的新版运行时；而 @Native code asset 的加载路径不同，最终解析到系统旧版 DLL，导致 ABI 冲突。

### 7.4 修复

为 `dcb_shared` 目标强制使用 **静态 CRT（/MT）**：

```cmake
# dart/native/CMakeLists.txt
if(WIN32)
  set_property(TARGET dcb_shared PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
```

编译后 DLL 不再依赖外部 MSVCP140.dll / VCRUNTIME140.dll，所有 CRT 代码（含 mutex）内联到 DLL 中。

### 7.5 验证

- `dart/test/ffi_native_test.dart`（@Native 冒烟）：通过
- `examples/base_demo`（DynamicLibrary.open 兼容路径）：79 测试通过
- `examples/codegen_demo`：62 测试通过
- `examples/hook_demo`（纯 @Native）：3 测试通过

### 7.6 教训

- Windows 上如果系统存在旧版 VC Redist，**任何**使用 C++ 标准库线程/互斥设施的 DLL 经 @Native 加载都可能崩溃。
- 对于通过 Native Assets hook 分发的共享库，**强烈建议**使用 `/MT` 静态 CRT，避免对终端用户环境的隐式依赖。
- `DynamicLibrary.open` 与 @Native 的 DLL 搜索路径不同，同一 DLL 在两条路径下行为可能不一致。

---

## 8. 【环境依赖】Windows 上 Visual Studio 未将 CMake 加入 PATH

### 8.1 现象

在 Windows 上执行项目文档中的 `cmake` 命令时，PowerShell / CMD 可能提示找不到 `cmake`：

```powershell
cmake : 无法将“cmake”项识别为 cmdlet、函数、脚本文件或可运行程序的名称。
```

但 Visual Studio 安装目录下其实自带 CMake，例如：

```text
C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

### 8.2 原因

Visual Studio 安装器默认不会把 CMake 添加到系统 `PATH`。只有单独安装的 CMake（例如从 cmake.org 下载）或某些工作负载才会注册到 PATH。

### 8.3 临时解决

使用 VS 自带的 CMake 绝对路径，或切换到 Developer PowerShell / Developer Command Prompt for VS：

```powershell
# 使用 VS 自带 CMake 的绝对路径
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

或在 VS 的 **Developer PowerShell for VS 2026** 中执行，该环境已配置好 CMake 路径。

### 8.4 彻底解决

单独安装 CMake（>= 3.24）并确保安装程序勾选"Add CMake to the system PATH for all users / current user"。

---

## 9. 【已解决】CI 上 codegen 生成容器类型退化为 i32

### 9.1 现象

CI（GitHub Actions）上运行 `dcb_gen generate` 后，生成的 `wire_dispatch.cpp` 编译失败：

```text
error C2664: 'async_simple::coro::Lazy<int32_t> demo::api::sum_scores(std::unordered_map<...>)':
cannot convert argument 1 from 'int' to 'std::unordered_map<...>'

error C2664: 'void dcb::ByteWriter::i32(int32_t)':
cannot convert argument 1 from 'async_simple::coro::Lazy<int32_t>' to 'int32_t'
```

但本地运行相同的 codegen 命令生成的代码完全正确。

### 9.2 根因

**CI 流程顺序问题**：CI 在 cmake 之前运行 codegen：

```yaml
# .github/workflows/push-build.yml
- name: codegen 生成 + 构建 + 测试
  run: |
    dart run bin/dcb_gen.dart generate ../examples/codegen_demo/dart_cpp_bridge.yaml
    cmake -S . -B build  # <-- codegen 时 build/_deps 还不存在！
```

**依赖链**：用户 API 头文件间接包含 async_simple 头文件：

```text
bridge_api.h
  → dart_cpp_bridge/stream_sink.hpp
    → dart_cpp_bridge/session.hpp
      → dart_cpp_bridge/runtime.hpp
        → <async_simple/Future.h>   ← 缺失！
        → <async_simple/Promise.h>  ← 缺失！
        → <async_simple/Unit.h>     ← 缺失！
```

**类型退化机制**：当 libclang 无法解析这些头文件时，产生的解析错误会导致后续模板类型（如 `std::vector<T>`、`std::unordered_map<K,V>`）在 AST 中退化为 `int`。IR 中记录的类型变成 `{"kind": "i32"}` 而非正确的容器类型，最终生成的代码传入 `int` 而非容器。

**本地为何正常**：本地曾运行过 cmake，`build/_deps` 目录存在，包含 FetchContent 下载的完整 async-simple/asio 头文件。

### 9.3 解决方案

补全 `dcb_gen_tool/stubs/async_simple/` 目录下的 stub 头文件，使 codegen 完全独立于 cmake 构建：

```text
dcb_gen_tool/stubs/async_simple/
├── Executor.h          # 更新：补充 Context/ScheduleOptions/ExecutorStat/IOExecutor/Slot
├── Future.h            # 新增
├── Promise.h           # 新增
├── Unit.h              # 新增
└── coro/
    ├── Lazy.h          # 已有
    └── FutureAwaiter.h # 新增
```

这些 stub 提供最小化的类型定义，仅满足 libclang 解析所需，不包含实际实现。

### 9.4 示例：stub 头文件

`async_simple/Future.h`：

```cpp
#pragma once
// Parse-only stub for codegen (real build uses FetchContent async-simple).
#include <exception>
#include <utility>

namespace async_simple {

template <typename T>
class Future {
 public:
  Future() = default;
  Future(Future&&) = default;
  Future& operator=(Future&&) = default;

  bool valid() const { return true; }
  T& value() { return val_; }
  const T& value() const { return val_; }

 private:
  T val_{};
};

}  // namespace async_simple
```

`async_simple/Executor.h`（需包含 `AsioExecutor` 使用的全部类型）：

```cpp
#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace async_simple {

class IOExecutor;
class Slot;
using Context = void*;

struct ScheduleOptions {
  bool prompt = true;
};

struct ExecutorStat {
  size_t pendingTaskCount = 0;
};

class Executor {
 public:
  using Func = std::function<void()>;
  using Duration = std::chrono::nanoseconds;

  enum class Priority : uint8_t { YIELD = 0, DEFAULT = 1, HIGH = 2 };

  virtual ~Executor() = default;
  virtual bool schedule(Func func) = 0;
  virtual bool schedule(Func func, uint64_t schedule_info);
  virtual bool checkin(Func func, Context, ScheduleOptions);
  virtual void* checkout();
  virtual ExecutorStat stat() const;
  virtual IOExecutor* getIOExecutor();
  virtual bool currentThreadInExecutor() const;
  virtual size_t currentContextId() const;

 protected:
  virtual void schedule(Func func, Duration dur, uint64_t, Slot*);
};

class IOExecutor { public: virtual ~IOExecutor() = default; };
class Slot { public: virtual ~Slot() = default; };

}  // namespace async_simple
```

### 9.5 验证

移除 `build/_deps` 后运行 codegen，IR 中容器类型正确：

```python
# 验证脚本
import json
ir = json.load(open("native/generated/ir.json"))
fn = [f for f in ir["functions"] if f["name"] == "sum_scores"][0]
assert fn["args"][0]["type"]["kind"] == "map"  # 而非 "i32"
```

生成的 `wire_dispatch.cpp` 正确使用容器解码：

```cpp
// 正确 ✓
const auto scores = r.map<std::string, std::int32_t>(...);

// 错误 ✗（修复前）
const auto scores = r.i32();
```

### 9.6 教训

- **codegen 必须独立于构建系统**：codegen 工具不应依赖 cmake FetchContent 的产物，stubs 必须完整覆盖所有间接依赖。
- **libclang 解析错误会级联**：即使错误发生在无关的头文件中，也可能导致后续模板类型退化。`-ferror-limit=0` 只能防止错误数量截断，不能防止类型退化。
- **CI 与本地环境差异**：本地残留的构建产物（如 `build/_deps`）可能掩盖问题，CI 的干净环境反而能暴露依赖缺失。

