# 已知问题与技术债

> 记录实现过程中已确认的卡点，避免重复踩坑。  
> 更新日期：2026-08-04

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
| `kCallDartHello` | io 上 `co_await cb(...)` |

阻塞上下文使用 `syncAwait(dcb::spawn(cb(...)))`；禁止在 io 线程上 syncAwait。

### 1.3 历史踩坑（保留备查）

| 尝试 | 现象 | 判断 |
|------|------|------|
| `async_simple::Promise` + FutureAwaiter | 30s 超时 | 与 executor 完成约定不匹配 |
| 仅 `asio::post(io, setValue)` | 仍不稳 | 未走 Lazy 的 `coAwait(Executor*)` |
| 裸 `coroutine_handle::resume` | AV | 不能当标准 coro 乱 resume |
| pool + `get()` | 能通但不干净 | 已替换为 oneshot |
| `spawn_on_asio` 里 coroutine lambda capture | gen=0 / AV | factory 在 `start()` 后销毁，capture 悬空；须 `shared_ptr` 保活到 Lazy 结束 |
| 成员函数 coroutine 读 `this->field` | 偶发错值 | DartFn 改为静态 Lazy，参数 by-value |

### 1.4 相关代码

- `include/dart_cpp_bridge/channel.hpp`
- `include/dart_cpp_bridge/asio_executor.hpp`
- `include/dart_cpp_bridge/dart_fn.hpp` — `operator()` 仿函数（返回 `Lazy<Ret>`）
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

- DartFn 仅提供异步 `operator()`（返回 Lazy）。  
- 阻塞场景用户自行 `syncAwait(dcb::spawn(fn(...)))`，在 io 线程上调用会自死锁 → **用户问题**。  
- 文档与 API 注释需持续强调，避免误用。

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

CI（GitHub Actions）上运行 `dcb_gen_tool generate` 后，生成的 `wire_dispatch.cpp` 编译失败：

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
    dart run bin/dcb_gen_tool.dart generate ../examples/codegen_demo/dart_cpp_bridge.yaml
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

---

## 10. 【已绕过】MSVC 19.51 协程 lambda 捕获变量损坏

### 10.1 现象

在 ForeignExecutor 上使用 `.via(ex).start()` 启动协程时，协程 lambda 中捕获的变量（`std::string`、`DartFn`、`shared_ptr` 等）在协程恢复后变成垃圾值，导致 ACCESS_VIOLATION 崩溃：

```text
===== CRASH =====
ExceptionCode=-1073741819
pc 0x00007ffc... dart_cpp_bridge.dll+0x51fa2
```

调试输出显示捕获的 `std::string` 变成乱码：

```text
[DBG] lazy started, input=?g?    ← 应为 "hello"
```

### 10.2 触发条件

```cpp
// ✗ 崩溃：协程 lambda 捕获
ex->schedule([cb = std::move(callback), input = std::move(input), ex]() mutable {
  auto lazy = [cb = std::move(cb), input = std::move(input)]()
      -> async_simple::coro::Lazy<> {
    auto result = co_await cb(input);  // cb 和 input 已损坏！
    // ...
  }();
  std::move(lazy).via(ex).start([](auto&&) {});
});
```

关键要素：
- **MSVC 19.51**（VS 2026）编译器
- 协程 lambda（`[]() -> Lazy<> { co_await ...; }`）
- 捕获列表中含 move-only 或非 trivial 类型（`std::string`、`DartFn`、`shared_ptr`）
- 协程被 `.via(ex).start()` 调度到另一个执行上下文

### 10.3 根因

MSVC 19.51 对协程 lambda 的捕获处理存在 bug：协程帧（coroutine frame）未正确复制/移动 lambda 的捕获变量。当协程被调度到另一个线程恢复时，捕获变量已经是悬空引用或未初始化的内存。

注意：同样的代码在 GCC/Clang 上可能正常工作，这是 MSVC 特有的问题。

### 10.4 解决方案

使用独立的 **static 协程函数**，通过函数参数传递所有变量（而非捕获）：

```cpp
// ✓ 正确：static 协程函数 + 参数传递
static async_simple::coro::Lazy<> my_coro(
    std::shared_ptr<co::oneshot::Sender<std::string>> tx_ptr,
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  auto result = co_await cb(input);  // 参数完好
  tx_ptr->send(std::move(result));
  co_return;
}

// 调用处：普通 lambda（非协程）负责调用 static 函数
ex->schedule([tx_ptr, cb = std::move(callback), input = std::move(input), ex]() mutable {
  my_coro(std::move(tx_ptr), std::move(cb), std::move(input))
      .via(ex)
      .start([](auto&&) {});
});
```

关键区别：
- 外层 lambda 是**普通 lambda**（非协程），捕获不受影响
- 内层协程是**命名函数**，变量通过参数传入，存储在协程帧的参数区域

### 10.5 影响范围

| 场景 | 是否受影响 |
|------|----------|
| 协程 lambda + `.via(ex).start()` | ✗ 受影响 |
| 协程 lambda + `syncAwait` | ✗ 受影响 |
| static 协程函数 + `.via(ex).start()` | ✓ 正常 |
| static 协程函数 + `syncAwait` | ✓ 正常 |
| 普通 lambda（非协程）捕获 | ✓ 正常 |
| codegen 生成的 wire_dispatch | ✓ 正常（已使用 static 协程函数） |

### 10.6 教训

- **MSVC + 协程 + lambda 捕获 = 危险组合**。在 MSVC 上始终使用命名协程函数 + 参数传递。
- codegen 生成的代码已经使用 static 协程函数模式（早期发现的 C2660 bug 促使采用此模式），因此不受影响。
- 此 bug 与 ForeignExecutor 无关，在任何 executor 上的协程 lambda 都可能触发。只是 ForeignExecutor 场景更容易暴露（跨线程调度）。

---

## 11. 【已解决】iOS/Android 集成测试中 @Native asset ID 无法解析

### 11.1 现象（已修复）

在 iOS simulator 上运行集成测试时，`DcbLib.init()` 抛出异常：

```text
Invalid argument(s): Couldn't resolve native function 'dcb_session_finalizer_ptr'
in 'package:dart_cpp_bridge/dart_cpp_bridge.dart' :
No asset with id 'package:dart_cpp_bridge/dart_cpp_bridge.dart' found.
Available native assets: package:codegen_demo/codegen_demo.dart.
```

### 11.2 根因

`dart_cpp_bridge` 包的 FFI 绑定层硬编码了 asset ID `package:dart_cpp_bridge/dart_cpp_bridge.dart`，
但 hooks 系统要求 code asset 必须注册在构建包自己的命名空间下，两者不匹配。

### 11.3 修复方案

采用方案 1（codegen 生成 @Native externals）：

1. **移除 `dart_cpp_bridge` 包中所有 `@Native` externals 和 `DynamicLibrary.open()` 路径**
2. **`NativeBindings` 变为纯数据类**：只持有函数指针，由下游包创建
3. **`DartCppBridge.init(bindings: ...)` 接受必需的 `NativeBindings` 参数**
4. **每个下游包生成自己的 `dcb_bindings.dart`**：包含 `@Native` externals，
   assetId 指向用户包（如 `package:codegen_demo/src/native_gen/dcb_bindings.dart`）
5. **hook 注册匹配的 asset name**：`assetName: 'src/native_gen/dcb_bindings.dart'`

这样 `@Native` 的 assetId 和 hook 注册的 asset 天然一致，无需跨包 remapping。

### 11.4 验证

- macOS `flutter test`：66/66 通过
- iOS simulator `flutter build ios --simulator`：成功
- iOS simulator 集成测试：通过（sync/async/normal/stream/dartfn/opaque 全部正常）

---

## 12. 【已解决】StreamSink / channel awaiter 的裸指针悬垂

- `StreamSink`：原先持 `Session*`，session 关闭（registry 最后引用释放）后
  `add()`/`end()`/`error()` 会解引用已销毁对象；现改为持 `shared_ptr<Session>`，
  关闭后靠 generation 检查静默丢弃晚到调用。
- `co::mpsc` / `co::oneshot`：`recv()` 的 awaitable/awaiter 原先持 `state_.get()` 裸指针，
  Receiver 在协程挂起期间被销毁/移动（或对临时 `Pair` 直接 `recv()`）会 UAF；
  现改为持 `shared_ptr<state>`，状态至少活到协程恢复。
- 相关：`include/dart_cpp_bridge/stream_sink.hpp`、`include/dart_cpp_bridge/channel.hpp`。

---

## 13. 【已解决】`dartfn_sender::opstate` 构造序：connect 期间 env 查询拿到空 scheduler

### 13.1 现象

stdexec 迁移后（`on_io` 糖层删除、改用 `stdexec::starts_on`），`examples/base_demo` 的 Dart 测试在 `Counter.callCallback`（DartFn 反向调用路径）完成后立即崩溃：

```text
===== CRASH =====
ExceptionCode=-1073741819 (0xC0000005, access violation)
pc 0x... dcb_base_demo.dll+0x28104
```

- `dcb_smoke.exe` 同样的 DartFn e2e 路径**不崩**（且能打印出 `get_env` 被调用、`op=null`）；
- 单独跑 `sleepAndGet` 等后续测试不崩，只有经 `starts_on` 包装的 dartfn 链崩，稳定复现。

### 13.2 根因

`dartfn_sender::opstate` 构造函数中，`ctl_->op = this` 写在成员初始化**之后**的构造体里：

```cpp
opstate(const IoContextScheduler* sched, co::oneshot::Receiver<DartFnReply> rx, ...)
  : ..., ctl_(std::make_shared<dartfn_ctl<opstate>>()),      // op == nullptr
    inner_(stdexec::connect(std::move(rx), inner_rcvr_t{ctl_})) {
  ctl_->op = this;   // ← 太晚！connect 已经在读 ctl_->op
}
```

stdexec 的 `connect_t::operator()` 在 connect 期间**无条件**调用 `get_env(receiver)`（用于 `transform_sender` / completion domain 计算）。`dartfn_inner_receiver::get_env()` 读 `ctl_->op->sched_`，此时 `ctl_->op` 还是 `nullptr`，于是返回一个带 **null scheduler 的 `sched_env`**。

- smoke 路径：这个 null env 只是被传入 `transform_sender(oneshot_rx, env)`，oneshot rx 是自定义 sender、不查询 env 里的 scheduler，**不崩溃**；
- Dart 路径：dartfn 链被 `starts_on`（内部展开为 `__sequence(continues_on(just(), sched), child)`）包裹，receiver 是带 `__sched_env` 的 stdexec 内部 receiver，env 里的 null scheduler 被真正解引用 → AV。

### 13.3 调试路径

| 手段 | 结论 |
|------|------|
| 单独跑 `-n "Counter"` / `-n "sleepAndGet"` | 稳定复现/排除；确认与顺序无关、与 starts_on 包装有关 |
| `RelWithDebInfo` + cdb `-o` 跟随子进程 + `sxe av` | 崩溃栈定位到 `dartfn_inner_receiver::get_env` ← `connect_t` ← `opstate` 构造函数 |
| 在 `get_env` 加临时 `fprintf` | smoke 路径确实以 `op=null` 调用且不崩 → 差异在 env 消费方，不在调用方 |

### 13.4 修复

`ctl_->op` 在构造 `inner_`（connect）**之前**就指向 `this`。`sched_` 成员在 `ctl_`/`inner_` 之前初始化，get_env 读 `ctl_->op->sched_` 时数据已就绪：

```cpp
opstate(const IoContextScheduler* sched, co::oneshot::Receiver<DartFnReply> rx,
        Rcvr rcvr, DecodeRet decode)
  : sched_(sched),
    rcvr_(std::move(rcvr)),
    decode_(std::move(decode)),
    ctl_(make_ctl(this)),
    inner_(stdexec::connect(std::move(rx), inner_rcvr_t{ctl_})) {}

static std::shared_ptr<dartfn_ctl<opstate>> make_ctl(opstate* self) {
  auto c = std::make_shared<dartfn_ctl<opstate>>();
  c->op = self;
  return c;
}
```

（move 构造函数里 `ctl_->op = this` 的刷新逻辑保持不变。）

### 13.5 验证

- `dcb_smoke.exe`：14 项全过
- `examples/base_demo` Dart 测试：79/79 通过

### 13.6 教训

- **stdexec 的 `connect_t` 会在 connect 期间查询 receiver env**（`get_env` + `transform_sender`），自定义 sender 的 opstate 构造函数中，任何在 connect 之后才初始化的指针都不能被 inner receiver 的 `get_env()` 依赖。
- 同样的 `connect` 调用在裸 receiver（smoke）与 stdexec 算法 receiver（`starts_on`/`__sequence` 内部 receiver）下行为不同：前者 env 里的坏值可能从不被读取，后者（`__sched_env`）会立即解引用。**本地最小复现（smoke）通过不代表 Dart 全链路安全。**
- 该 bug 在迁移分支上早已存在（构造函数顺序从未对过），只是此前 `on_io`/`start_on_io` 路径从未经 stdexec 算法 receiver 连接 dartfn，迁移到 `starts_on` 后首次暴露。



---

## 14. 【已解决】stdexec（nvhpc-26.05）自定义 scheduler/sender 的编译期陷阱

> 背景：feat/stdexec-migration 分支将 runtime 从 async-simple 迁移到 stdexec
> （P2300 参考实现，锁 
vhpc-26.05 tag，MSVC 19.51 / VS2026）。以下问题均为
> 自定义 scheduler / sender 在 MSVC 下遇到的编译期失败，全部已解决，列此备查。

### 14.1 自定义 sender 必须提供 connect 成员

- **现象**：自写的 schedule_sender（照抄官方 inline_scheduler 样板但漏了 connect）
  作为 child 参与 continues_on 等组合时，stdexec::connect_t 报
  C3889: 对类类型 connect_t 的对象的调用未找到匹配的调用运算符。
- **根因**：__connectable_to 要求 sender 满足 __with_static_member /
  __with_member / __with_co_await / __with_legacy_tag_invoke 之一；
  自定义 sender 只有 sender_concept + completion_signatures 不够，还必须提供
  connect(receiver)（成员或 __static_connect）。
- **修复**：照 inline_scheduler 样板补 connect 成员。注意 sender 若持有成员
  数据（如 sched_），connect 必须是**非静态成员**（&& 限定），因为要读取
  	his 的数据。

### 14.2 scheduler 概念要求可复制；std::atomic 成员会破坏它

- **现象**：AsioScheduler 含 std::atomic<std::thread::id> 成员（io 线程 id 追踪），
  导致类不可复制 → 不满足 stdexec::scheduler 概念 → 组合全部编译失败。
- **修复**：把 atomic 移入 shared_ptr<State>，scheduler 变为可复制。
- **衍生坑**：defaulted operator== 会逐成员比较，sio::io_context 没有
  operator== → 编译失败。须手写 operator==（比较 io_context 地址 + shared_ptr）。

### 14.3 schedule() 必须是 const 成员

- **现象**：schedule() 非 const 时，continues_on 的完成签名计算
  （schedule_result_t<Scheduler>）失败 → sender_in 概念失败。
- **修复**：schedule()/schedule_at() 声明为 const noexcept；sender 里存
  const Scheduler*（通过 const 对象的引用成员 io_context& 仍可 post，无碍）。

### 14.4 自定义 sender 的 env 需要 __get_completion_behavior 查询

- **现象**：	hen/let_value 等适配器组合自定义 sender 时，__sync_attrs 查询
  child 的 __get_completion_behavior_t<set_value_t>，缺失则组合失败。
- **修复**：照 inline_scheduler 的 __inline_attrs 样板，给 sender 加
  get_env() 返回提供该查询的 attrs 结构（返回 __inline_completion）。

### 14.5 stdexec 适配器（then/let_value/...）只接受 sexpr（basic-sender）child

- **现象**：自定义 sender | stdexec::then(...) 报
  __then.hpp(57): static_assert(__sender_for<_Sender, then_t>) 失败。
- **根因**：0.11 的适配器基于 sexpr + transform_sender 机制，非 sexpr 的
  自定义 sender 不能作为 child 组合。
- **对策**：自定义 sender（on_scheduler_sender、dartfn_sender）只作为
  **最外层** sender 使用；需要后续处理的逻辑（如 DartFn 的 decode）放进 sender
  内部（inner receiver 完成时处理），不再 | then(...)。

### 14.6 sexpr opstate 不可移动：emplace / make_unique 强制 move 会编译失败

- **现象**：std::optional<op_t>::emplace(connect(...)) 与
  std::make_unique<op_t>(connect(...)) 都报
  C2665: opstate 没有重载函数可以转换所有参数。
- **根因**：sexpr 的 opstate 是 immovable（STDEXEC_IMMOVABLE）；mplace/
  make_unique 的 std::forward 会把 prvalue 变成 xvalue 强制走 move 构造。
- **修复**：opstate 成员**就地构造**（构造函数的成员初始化列表里直接
  op(connect(...))，靠 guaranteed elision，不触发 move）。
- **衍生坑**：connect_result_t 与 decltype(connect(...)) 可能有 cv/引用差异，
  存 opstate 的类型要用 decltype(stdexec::connect(...)) 精确匹配，否则同样报
  C2665（MSVC 下 connect_result_t 对自定义 sender 的求值不可靠）。

### 14.7 其它零散编译期约束

| 问题 | 说明 |
|------|------|
| stdexec::start 要求 **lvalue** opstate | start_t::operator()(_Op&)；start(connect(...)) 临时对象编译失败，必须 uto op = connect(...); start(op); 两步 |
| sync_wait 返回 optional<tuple<...>> | 单值也要 std::get<0>(*v)，不是 optional<T> |
| receiver 的 set_value 必须 noexcept | stdexec 有 static_assert；oneshot 完成若从 const& 拷贝值（如 DartFnReply 含 vector/string，拷贝非 noexcept）会触发，完成值必须 **move 传递** |
| start_detached 编译期拒绝带 set_error 的 sender | __never_sends<set_error_t> 约束；continues_on 等总会引入 set_error_t(exception_ptr)（分配失败）→ 自定义 receiver 吞错误 |
| auto 返回类型的成员函数不能跨 TU 定义 | C3779；invoke_dart_fn_async 从 sender auto 改为显式返回类型 |
| 模板参数顺序 | un_async<T, S, Encode> 显式给 T 时 S 必须可推导，写反会全部 C2672 |
| 局部类不能有成员模板 | fire-and-forget receiver 的模板 set_value 必须在命名空间作用域 |

### 14.8 调试方法备忘

MSVC 的概念失败信息极差（只有 C3889/C2338，无约束链）。有效手段：
- **独立探针工程**（只 include 头文件、不链接 runtime），二分最小复现；
- static_assert(stdexec::sender_to<S, R>) 逐个概念细分（sender / receiver /
  sender_in / sender_to 分开断言）；
- 	ypeid(T).name() 打印 sender 类型，对比 connect_result_t 与
  decltype(connect(...)) 是否一致。

---

## 15. 【已绕过】MSVC 下 stdexec 算法（continues_on / on / exec::task）运行期不可用

### 15.1 continues_on / on：connect 后运行期 AV

- **现象**：continues_on(oneshot_rx, sched) + connect + start 后，send 触发
  完成时直接 access violation（0xC0000005），探针可稳定复现；编译期完全正常。
- **定位**：完成链进入 schedule_from/sexpr 内部 receiver 时崩溃，疑似 MSVC
  对 stdexec 0.11 sexpr/transform 机制的运行期布局问题（未深挖到根因）。
- **对策**：**手写 on_scheduler_sender 包装**（connect child + inner receiver +
  sio::post 回目标调度器），只依赖 connect/set_value/start 稳定 API，
  运行期验证通过。continues_on 从 runtime 中移除。

### 15.2 xec::task（协程）与自定义 env 不兼容

- **现象**：on_io(exec::task<int>{...}) 编译失败：
  get_completion_signatures(task, env) 无匹配重载。
- **根因**：task 的完成签名计算依赖 env 提供 get_scheduler/
  get_start_scheduler（__start_scheduler_type），即便注入 scheduler 仍失败
  （nvhpc-26.05 的 task 对 env 的约束过紧，MSVC 下不可用）。
- **对策**：smoke 全部改 receiver/sender 组合（不再用协程）；stream.hpp 的
  xec::task 接口保留但**未被实例化**（未使用则不报错）——后续若要用协程，
  需升级 stdexec 版本或换实现。

### 15.3 影响面

| 组件 | 状态 |
|------|------|
| 手写 on_scheduler/on_io/dartfn_sender | ✓ 运行期验证通过 |
| stdexec::continues_on / stdexec::on | ✗ 弃用（MSVC AV） |
| xec::task 协程 | ✗ 弃用（签名计算失败） |
| stdexec::just / 	hen / write_env / sync_wait / stop_token | ✓ 正常 |

---

## 16. 【已解决】opstate 生命周期与指针悬挂（P2300 语义细节）

### 16.1 inner receiver 持裸 opstate 指针 → connect 返回链 move 后悬挂

- **现象**：DartFn 反向调用"卡死"（oneshot send() 返回 true 但无任何后续），
  无崩溃；加日志发现 settle 时 waiter == nullptr。
- **根因**：stdexec::connect 的返回链经过 __declfn 包装，**不保证 guaranteed
  elision**——opstate 可能被 move，而 inner receiver 持有的 Op* 仍指向
  构造时的旧地址（旧对象已析构）→ use-after-free（未崩是因为内存未复用）。
- **修复**：inner receiver 不存裸指针，改存 **shared control block**
  （op_ctl<Op>{ Op* op; }），opstate 的 move 构造里刷新 ctl_->op = this。
- **衍生坑（构造序）**：ctl_->op = this 必须在 inner_（connect）**之前**
  就位——stdexec 的 connect_t 会在 connect 期间调用 get_env(receiver)，
  inner receiver 的 get_env() 读 ctl_->op->sched_，晚初始化会拿到
  null scheduler（smoke 路径不崩、starts_on 路径 AV，见第 13 节）。

### 16.2 fire-and-forget 场景 opstate 未完成即析构 → 回复静默丢失

- **现象**：dispatch（DartFn）回复"卡死"；oneshot 的 opstate 析构函数把
  waiter 注销，send() 时无 waiter → 值被静默丢弃。
- **根因**：P2300 要求 opstate 在 start() 后必须存活到完成信号发出；局部
  uto op = connect(...) 在函数（如 io 上执行的 factory）返回时析构，而
  业务 sender（DartFn）尚未完成。
- **修复**：dcb::start_detached(sndr, rcvr) —— 堆分配 af_state
  （opstate 成员就地构造 + rcvr 包装持 self 引用），完成时 self.reset()
  释放，未完成时由 self 循环引用保活（fire-and-forget 的代价：永不完成的
  sender 会泄漏）。
