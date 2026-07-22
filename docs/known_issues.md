# 已知问题与技术债

> 记录实现过程中已确认的卡点，避免重复踩坑。  
> 更新日期：2026-07-22

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

### 5.4 彻底解决

安装最新版本的 **Visual C++ Redistributable**（14.40+，即 VC145 运行时），确保系统 `MSVCP140.dll` 版本不低于构建时使用的版本。

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

## 7. 【环境依赖】Windows 上 Visual Studio 未将 CMake 加入 PATH

### 7.1 现象

在 Windows 上执行项目文档中的 `cmake` 命令时，PowerShell / CMD 可能提示找不到 `cmake`：

```powershell
cmake : 无法将“cmake”项识别为 cmdlet、函数、脚本文件或可运行程序的名称。
```

但 Visual Studio 安装目录下其实自带 CMake，例如：

```text
C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
```

### 7.2 原因

Visual Studio 安装器默认不会把 CMake 添加到系统 `PATH`。只有单独安装的 CMake（例如从 cmake.org 下载）或某些工作负载才会注册到 PATH。

### 7.3 临时解决

使用 VS 自带的 CMake 绝对路径，或切换到 Developer PowerShell / Developer Command Prompt for VS：

```powershell
# 使用 VS 自带 CMake 的绝对路径
& "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
```

或在 VS 的 **Developer PowerShell for VS 2026** 中执行，该环境已配置好 CMake 路径。

### 7.4 彻底解决

单独安装 CMake（>= 3.20）并确保安装程序勾选“Add CMake to the system PATH for all users / current user”。

