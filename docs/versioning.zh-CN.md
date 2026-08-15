# 文档版本说明

`dart_cpp_bridge` 目前存在两代 C++ 异步模型。这里的 v1/v2 是文档和
C++ 实现代际，不等同于当前已经发布的 pub 包版本。下一代仍处于开发中，
尚未发布为 `dart_cpp_bridge: ^2.0.0`。

## 一览

| 文档版本 | 发布状态 | C++ 异步模型 | 适用场景 |
| --- | --- | --- | --- |
| **v1** | 已发布的 1.x，当前发布线为 1.3.0 | `async_simple::coro::Lazy`、`Executor`、`Signal` / `Slot` | 维护现有 v1 应用或旧生成代码 |
| **v2** | 下一主版本，当前 `feat/stdexec-migration` 开发线 | stdexec sender、scheduler、`stdexec::task`、stop token | 使用当前仓库开发或准备迁移到 v2 |

两代都保持 Dart 公共 API、C ABI、wire 协议、生成文件布局和 method ID
规则等兼容性契约不变。v2 的主要变化集中在 C++ 业务代码的异步写法。

文档站使用 `starlight-versions` 插件：v2 作为根路径下的当前文档，v1
归档在 `/v1/` 下，并通过插件提供的版本选择器切换。

## v1 — 已发布的 1.x

v1 是 1.x 发布版本使用的模型：

- 异步函数返回 `async_simple::coro::Lazy<T>`；
- 使用 `async_simple::Executor` 和 `.via(...)` 调度；
- 使用 `Signal` / `Slot` 做协作式取消；
- 外部事件循环通过 `ForeignExecutor` / `foreign_runtime.h` 接入；
- codegen 识别 async-simple 协程返回类型。

当项目依赖已发布的 1.x 包时，请使用 v1 文档。文档站中的
async-simple 页面会作为 v1 归档保留。

## v2 — 当前开发线

当前分支已经完成 async-simple 到 stdexec 的迁移：

- 异步函数返回 `stdexec::task<T>` 或其他 stdexec sender；
- 内置 Asio 事件循环通过 `stdexec::scheduler` 暴露；
- 使用当前名称 `stdexec::starts_on`、`stdexec::on`、
  `stdexec::continues_on`、`stdexec::sync_wait`；
- 取消统一使用 `inplace_stop_source` / `stop_token`；
- 外部事件循环提供普通 stdexec scheduler，示例见
  `examples/foreign_runtime_demo/native/uv_scheduler.hpp`；
- 生成的异步 dispatch 使用零捕获协程 IIFE，确保懒协程状态由协程帧持有；
- `dcb_gen_tool` 能解析 `stdexec::task` 并生成 v2 dispatch 形态。

v2 还没有发布为正式 pub 包。在正式发布前，不要把 v1 项目的依赖直接改成
`^2.0.0`；如需试用，请显式固定仓库 revision，并使用同一 revision 的工具
重新生成代码。

## 迁移对照

| v1 | v2 |
| --- | --- |
| `async_simple::coro::Lazy<T>` | `stdexec::task<T>` 或 sender |
| `Executor` / `.via(ex)` | scheduler + `starts_on` / `on` |
| `syncAwait(lazy)` | `stdexec::sync_wait(sender)` |
| `collectAll` / `collectAny` | `when_all` / `exec::when_any` |
| `Signal` / `Slot` | stop source / stop token |
| `ForeignExecutor` | 用户提供的 stdexec scheduler |

v2 的完整规则和已编译验证的示例见
[`docs/cpp26_executor_model_usage.md`](./cpp26_executor_model_usage.md)。
