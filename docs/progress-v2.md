# v2 当前实现进度

> 更新日期：2026-08-14  
> 当前开发线：`feat/stdexec-migration`  
> 发布状态：v2 尚未发布；Dart 包和 `dcb_gen_tool` 的发布版本仍为 1.3.0

## 总览

| 区域 | 状态 | 说明 |
| --- | --- | --- |
| C++ Runtime | ✅ | Asio `io_context` + `IoContextScheduler` + blocking scheduler |
| Async business API | ✅ | `stdexec::task<T>` / stdexec sender |
| Channels | ✅ | `co::oneshot` / `co::mpsc`，stop-token aware |
| Cancellation | ✅ | `inplace_stop_source` / `stop_token`，协作式传播 |
| Codegen parser | ✅ | 识别 `stdexec::task`，生成 v2 async dispatch |
| Generated dispatch | ✅ | 零捕获 coroutine IIFE，参数进入 coroutine frame |
| Foreign runtime | ✅ | 外部 loop 通过普通 stdexec scheduler 接入 |
| Dart API / C ABI / wire | ✅ | 继续遵守 v1 稳定兼容性契约 |
| v2 package release | ⏳ | 等待迁移验证、文档和发布准备完成 |

## v1 → v2 迁移边界

业务 C++ 异步签名需要迁移：

```text
async_simple::coro::Lazy<T>  →  stdexec::task<T>
Executor / .via(ex)          →  scheduler + starts_on / on
syncAwait(lazy)               →  dcb::sync_wait(sender)
Signal / Slot                 →  stop source / stop token
ForeignExecutor               →  application-provided stdexec scheduler
```

以下契约不变：Dart 公开 API、C ABI、wire frame 字段和消息类型、已有
method ID、生成文件名及 Native Assets 使用方式。

## 已验证入口

- `docs/cpp26_executor_model_usage.md`：stdexec 使用规则和编译验证示例；
- `examples/base_demo`：C++ Runtime / wire smoke tests；
- `examples/codegen_demo`：代码生成 fixture；
- `examples/foreign_runtime_demo`：libuv `UvScheduler` fixture；
- `dcb_gen_tool/scripts/parse_api.py`：`stdexec::task` 检测和 v2 生成模板；
- `dart/native/include/dart_cpp_bridge/runtime.hpp`：Runtime scheduler 入口。

## 验证命令

```powershell
# C++ base smoke test
cmake -S examples/base_demo -B examples/base_demo/build
cmake --build examples/base_demo/build --config Release
.\examples\base_demo\build\Release\dcb_smoke.exe

# Dart package
cd dart
puro dart test

# Documentation site
cd ..\docs-site
pnpm build
```

## 发布前清单

1. 保持 `dart/pubspec.yaml` 与 `dcb_gen_tool/pubspec.yaml` 版本同步，并为
   v2 编写对应 changelog。
2. 用 v2 工具重新生成两个 fixture，确认生成物不再包含 v1 async-simple 类型。
3. 在 Windows、Linux、macOS、Android、iOS 上验证 Native Assets hook。
4. 复核 C ABI、wire protocol、method ID 和生成文件名没有破坏性变化。
5. 将文档站默认版本从“v2 development”切换为正式 v2 release。
