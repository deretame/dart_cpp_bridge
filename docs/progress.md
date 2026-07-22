# dart_cpp_bridge 实现进度

> 对照设计文档：[frb_and_cpp_bridge_design.md](./frb_and_cpp_bridge_design.md)  
> **已知问题 / 技术债**：[known_issues.md](./known_issues.md)  
> 更新日期：2026-07-22

---

## 1. 总览

| 阶段 | 状态 | 说明 |
|------|------|------|
| **Phase 1** 手写骨架 | **基本完成** | Runtime / Session / 四通道 / DartFn io 真挂起 / Dart 包 / 测试 |
| **Phase 2** Codegen | 未开始 | 远端固定版 Python + libclang，手动触发 |
| **Phase 3** Native Assets + 生产 | 未开始 | hook 只编链接；错误表、压测等 |
| **Phase 4** 业务接入 | 未开始 | 不替换任何已有 FRB 生产桥 |

当前仓库是 **独立实验工程**，与 Breeze 等业务仓解耦。

Dart 测试：`cd dart && dart test`（约 **38** 例，含 DartFn sync/async 入口）。  
C++ 冒烟：`build/Release/dcb_smoke.exe`（oneshot 跨线程、io 不堵、DartFn e2e 模拟 reply）。

---

## 2. 已落地（相对设计 §0 锁定决策）

### 2.1 运行时与会话

| 设计项 | 实现 | 备注 |
|--------|------|------|
| asio `io_context` **单线程** | ✅ | `Runtime` |
| `AsioExecutor`（async_simple） | ✅ | `schedule` → `asio::post(io)` |
| `asio::thread_pool` + post | ✅ | normal / ticks 间隔 sleep |
| `spawn_blocking` → Lazy | ⚠️ 部分 | API 仍在；normal 路径多用 pool post |
| `spawn_on_asio` + `via(executor)` | ✅ | factory 保活到 Lazy 结束（防 coroutine lambda capture 悬空） |
| 长期 reply port + `request_id` | ✅ | |
| **Runtime 进程唯一** | ✅ | |
| **Session 每 Isolate 一个** | ✅ | `SessionRegistry` + `dcb_session_open` |
| dispose = generation，晚到 post 丢弃 | ✅ | |
| 不做 CancelToken | ✅ | |
| Stream 关订阅后 add 静默丢 | ✅ | `dcb_stream_close` |
| NativeFinalizer 自动关 session | ✅ | 对齐 FRB：日常可不手动 dispose |
| 可选 `dispose` / 进程 `shutdown` | ✅ | worker 勿调 shutdown |
| **DartFn 反向调用（参数式）** | ✅ | 见下表 |
| oneshot channel（`co::oneshot`） | ✅ | `include/dart_cpp_bridge/channel.hpp` |

#### DartFn 两种等待模式（不替用户兜底）

| C++ API | 行为 | 谁承担风险 |
|---------|------|------------|
| `callSync` / `callDartHelloSync` | **当前线程**阻塞直到 Dart reply | 若在 **io 线程**调用，会卡住调度器——用户自负 |
| `callAsync` / `callDartHello` | **io 上** `co_await` oneshot，**真挂起、不堵 io、不占 pool** | 需 Lazy 绑 `AsioExecutor`（`spawn_on_asio` 已保证） |

链路（对齐 FRB oneshot）：

```text
io:  post DartFnCall → co_await rx.recv()（挂起）
Dart Isolate: 回调 → dcb_dart_fn_reply → oneshot.send
io:  Executor::schedule(resume) → 继续
```

Dart 侧回调无论 sync/async 都在 **Isolate 事件循环**执行。细节与历史踩坑见 [known_issues.md](./known_issues.md) §1（**已解决**）。

### 2.2 通道与错误

| 通道 | Demo API | Dart | 测试 |
|------|----------|------|------|
| sync | `bridgeVersion` | `int` | ✅ |
| async（Lazy） | `add` / `echo` / `failAsync` | `Future` | ✅ |
| normal（线程池） | `sleepTest` | `Future` | ✅ |
| stream | `ticks` / `failStream` | `Stream` | ✅ |
| DartFn async（io 挂起） | `callDartHello` | `Future` + 闭包 | ✅ |
| DartFn sync（当前线程堵） | `callDartHelloSync` | `Future` + 闭包 | ✅ |
| wire 双 catch | 全部路径 | 抛 `StateError` | ✅ |
| 坏帧 / 未知 method / sync 误用 | — | — | ✅ |

### 2.3 Dart 包

| 项 | 状态 |
|----|------|
| `dart/` 纯 Dart 包 + FFI | ✅ |
| `DartCppBridge.init` 每 isolate | ✅ |
| Completer / StreamController 多路复用 | ✅ |
| 多 isolate async + stream | ✅ 测试覆盖 |
| Finalizer 自动 session_close | ✅ |

### 2.4 构建与工具

| 项 | 状态 |
|----|------|
| CMake + FetchContent asio / async-simple | ✅ |
| vendored `third_party/dart_api` | ✅（`cmake/fetch_dart_api.cmake`） |
| `dcb_smoke` 原生冒烟 | ✅（含 oneshot / DartFn e2e） |
| `dart test`（codec + FFI） | ✅ **38** 例量级 |
| 远端固定版 Python/libclang codegen | ⏳ | lock + 用户 cache（downloads/envs 按指纹）+ smoke；解析/生成未做 |
| Native Assets hook | ❌ |
| examples 用户模板 + PUBLIC 暴露依赖 | ⏳ 骨架有，未产品化 |

---

## 3. 与设计文档的差异 / 演进

设计原文偏「单 session + 长期 port」。实现中为支持**后台 Isolate 异步**，演进为：

```text
Runtime（进程唯一）
  └─ Session × N（每个调用 init 的 Isolate 一个 reply port）
```

- 业务仍不感知 port；wire 持有 `shared_ptr<Session>`。
- 生命周期：`init` 必调；**dispose 可选**（Finalizer 兜底）；`shutdown` 仅进程退出。

其余锁定决策（无取消、Dart 抛异常、codec 逻辑类型、BRIDGE 注解宏方向等）保持不变；codegen 宏与 libclang 流水线尚未实现。

---

## 4. 目录与入口

```text
docs/
  frb_and_cpp_bridge_design.md   # 设计全文
  progress.md                    # 本进度
  known_issues.md                # 技术债与已解问题
include/dart_cpp_bridge/         # 公共头（含 channel / asio_executor）
src/runtime|wire|ffi_entry       # 实现
dart/lib + dart/test             # Dart 包与测试
codegen/                         # Phase 2 预留
```

常用命令：

```powershell
# C++
cmake -S . -B build
cmake --build build --config Release
.\build\Release\dcb_smoke.exe

# Dart 测试（需先编出 dll）
cd dart
dart test
```

---

## 5. 下一步建议

1. **Phase 2**：bootstrap 已通 → 解析窄头（`bridge_api.h`）→ IR → 生成 wire/Dart。  
2. **Phase 3**：Native Assets hook；CMake export / examples。  
3. 可选：`spawn_blocking` 复用 oneshot 唤醒；Finalizer 说明、跨平台 CI。

---

## 6. 一句话

**Phase 1 手写桥已跑通四通道、多 Isolate、DartFn（io 真挂起 + sync 阻塞）与较完整测试；codegen/hook 未开始。**
