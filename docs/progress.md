# dart_cpp_bridge 实现进度

> 对照设计文档：[frb_and_cpp_bridge_design.md](./frb_and_cpp_bridge_design.md)  
> 更新日期：2026-07-22

---

## 1. 总览

| 阶段 | 状态 | 说明 |
|------|------|------|
| **Phase 1** 手写骨架 | **基本完成** | Runtime / Session / 四通道 / Dart 包 / 测试 |
| **Phase 2** Codegen | 未开始 | 远端固定版 Python + libclang，手动触发 |
| **Phase 3** Native Assets + 生产 | 未开始 | hook 只编链接；错误表、压测等 |
| **Phase 4** 业务接入 | 未开始 | 不替换任何已有 FRB 生产桥 |

当前仓库是 **独立实验工程**，与 Breeze 等业务仓解耦。

---

## 2. 已落地（相对设计 §0 锁定决策）

### 2.1 运行时与会话

| 设计项 | 实现 | 备注 |
|--------|------|------|
| asio `io_context` **单线程** | ✅ | `Runtime` |
| `asio::thread_pool` + post | ✅ | normal / ticks 间隔 sleep |
| `spawn_blocking` → Lazy | ⚠️ 部分 | API 仍在；normal 路径多用 pool post（避免 Future await 挂死） |
| 长期 reply port + `request_id` | ✅ | |
| **Runtime 进程唯一** | ✅ | |
| **Session 每 Isolate 一个** | ✅ | `SessionRegistry` + `dcb_session_open` |
| dispose = generation，晚到 post 丢弃 | ✅ | |
| 不做 CancelToken | ✅ | |
| Stream 关订阅后 add 静默丢 | ✅ | `dcb_stream_close` |
| NativeFinalizer 自动关 session | ✅ | 对齐 FRB：日常可不手动 dispose |
| 可选 `dispose` / 进程 `shutdown` | ✅ | worker 勿调 shutdown |

### 2.2 通道与错误

| 通道 | Demo API | Dart | 测试 |
|------|----------|------|------|
| sync | `bridgeVersion` | `int` | ✅ |
| async（Lazy） | `add` / `echo` / `failAsync` | `Future` | ✅ |
| normal（线程池） | `sleepTest` | `Future` | ✅ |
| stream | `ticks` / `failStream` | `Stream` | ✅ |
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
| `dcb_smoke` 原生冒烟 | ✅ |
| `dart test`（codec + FFI） | ✅ **32** 用例量级 |
| 远端固定版 Python/libclang codegen | ❌ |
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
  frb_and_cpp_bridge_design.md   # 设计全文（从 Breeze 同步）
  progress.md                    # 本进度
include/dart_cpp_bridge/         # 公共 C++/FFI 头
src/runtime|wire|ffi_entry       # 实现
dart/lib + dart/test             # Dart 包与测试
codegen/                         # Phase 2 预留
```

常用命令：

```powershell
# C++
cmake -S . -B build
cmake --build build --config Release

# Dart 测试（需先编出 dll）
cd dart
dart test
```

---

## 5. 下一步建议

1. **Phase 2**：`versions.lock` + 远端 Python/libclang + 解析窄头 → 生成 wire/Dart（替换手写 demo_api）。
2. **Phase 3**：Native Assets `hook/build.dart`；完善 CMake export / examples 模板。
3. 可选加固：`spawn_blocking` Lazy 路径稳定化与单测；Finalizer 行为专项说明；跨平台 CI。

---

## 6. 一句话

**Phase 1 手写桥已跑通四通道、多 Isolate 异步、自动 session 回收与较完整 Dart 测试；codegen 与 hook 尚未开始。**
