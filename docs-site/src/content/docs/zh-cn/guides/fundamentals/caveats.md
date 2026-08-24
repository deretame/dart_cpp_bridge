---
title: 注意事项与常见坑（v2）
description: v2 codegen、线程、取消、生命周期和 scheduler 的常见问题
---

:::note[v2.1.0]
本页按 stdexec 实现编写。已发布的 v1 项目请使用[版本文档](/dart_cpp_bridge/zh-cn/versions/)
和 v1 归档页面。
:::

## 代码生成

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 扫描头文件包含无法解析的依赖 | libclang 可能把模板类型静默降级，生成错误绑定 | 扫描头只 include 标准库、`dart_cpp_bridge/*` 和 `stdexec/execution.hpp`；重型 include 放入 `api_impl/*.cpp` |
| 使用类型别名或 `using namespace` | parser 可能无法解析公开类型 | 使用完整限定的具体类型 |
| 手工修改生成文件 | 下次生成会覆盖修改 | 修改 API 头或实现，然后运行 `dcb_gen_tool generate` |
| 以为 build hook 会自动生成 | Native Assets 只负责编译和链接 | API 签名变化后手动重新生成 |

## 线程与死锁

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 阻塞 io scheduler runner | 该 runner 在等待期间不可用；所有 runner 都阻塞时整个 scheduler 停顿 | 使用 `dcb::spawn_blocking` 或 `BRIDGE_NORMAL` |
| 在 io scheduler runner 调用 `dcb::sync_wait` | 包装器会拒绝；原始 `stdexec::sync_wait` 在所有 runner 等待同一 scheduler 时会死锁 | 只从 worker 或外部线程调用 |
| 在 `BRIDGE_SYNC` 中调用 DartFn | sync 调用阻塞时 Dart 无法回复 | 使用 `BRIDGE_ASYNC`、`BRIDGE_NORMAL` 或显式卸载 |
| 协程 lambda 捕获请求状态 | 懒协程恢复时可能已超过完整表达式生命周期 | 使用零捕获 IIFE，通过参数传递状态 |

## 取消与 Stream

| 坑 | 后果 | 正确做法 |
|---|---|---|
| 期待 Dart 强制取消 Future | Dart Future 无法打断原生工作 | 暴露 task ID，并传播 stdexec stop token |
| 取消 Stream 订阅 | Dart 停止接收，但原生工作可能继续 | 显式停止生产者，或接受后续 sink 调用被丢弃 |
| 还有 pending work 就销毁 scheduler | operation 可能仍持有 scheduler 引用 | 先 request stop，再排空结构化并发 scope |

## 生命周期

| 坑 | 后果 | 正确做法 |
|---|---|---|
| worker isolate 调用 `shutdown()` | 关闭全部 session 并停止进程级 Runtime | 只允许主 isolate 在进程退出时调用 |
| opaque 对象跨 isolate | handle 属于各自 Session | 对象只在所属 isolate 使用 |
| `dispose()` 后继续调用原生代码 | session 已关闭 | 重新 init，或在 dispose 前完成工作 |

## 纯 C API 与外部运行时

C bridge API 与 C++ 异步模型保持独立。C 侧只负责创建、完成和取消 operation；
C++ 调用方可以用 `dcb::async_wait` 等待结果。

libuv、glib 或自定义 loop 应实现普通 stdexec scheduler，不要重新引入 v1
的 `ForeignExecutor` 注册 API。

## 延伸阅读

- [Codegen 配置](/dart_cpp_bridge/zh-cn/codegen/configuration/)
- [v2 stdexec 异步 C++](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/)
- [内置运行时](/dart_cpp_bridge/zh-cn/guides/fundamentals/runtime/)
- [外部运行时集成](/dart_cpp_bridge/zh-cn/guides/advanced/foreign-runtime/)
