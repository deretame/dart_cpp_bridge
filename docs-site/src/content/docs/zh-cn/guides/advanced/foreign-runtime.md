---
title: 外部运行时集成（v2）
description: 让 libuv 或其他外部事件循环提供 stdexec scheduler
---

:::caution[v2 开发线]
v2 不再提供 `ForeignExecutor` 或 `foreign_runtime.h`。外部事件循环应
通过普通 stdexec scheduler 接入。旧 v1 注册 API 只保留在 v1 源码归档中。
:::

## v2 模型

bridge 负责 Dart 侧的 Asio Runtime。如果应用已经有 libuv、glib 或自定义
事件循环，应把该 loop 适配为 stdexec scheduler，再与 bridge sender 组合。

```text
应用自己的事件循环 worker
  ├─ 拥有 loop 线程
  ├─ 加锁进入任务队列
  ├─ 唤醒 loop
  └─ 在 loop 线程执行队列函数
          │
          ▼
UvScheduler / 自定义 stdexec::scheduler
          │
          ├─ schedule()       → 在 loop 执行一步 sender
          └─ schedule_after() → 可选的原生 timer
          │
          ▼
stdexec::task 与 bridge channel
```

完整参考实现见 `examples/foreign_runtime_demo/native/uv_scheduler.hpp` 和
`uv_worker.hpp`。它是普通 C++ 适配器，不创建 bridge-owned foreign executor，
也不需要 C 注册调用。

## 业务 API

被扫描的 API 头文件包含 stdexec execution 头，并返回
`stdexec::task<T>`：

```cpp
#pragma once

#include <dart_cpp_bridge/annotate.h>
#include <stdexec/execution.hpp>
#include <string>

BRIDGE_ASYNC
stdexec::task<std::string> ask_uv(std::string message);
```

生成的 dispatch 会在 bridge scheduler 上启动 task。业务协程中把属于外部
loop 的工作交给外部 scheduler，之后按 task/sender 组合规则回到 task 的 home
scheduler。

## Scheduler 的责任

外部 scheduler 必须：

- 可复制，并满足 stdexec scheduler 要求；
- `schedule()` 入队但不在调用线程内联执行用户代码；
- 入队后唤醒事件循环；
- 保证每个 continuation 恰好在 loop 线程执行一次；
- 在完成或取消前保持 queued operation 存活；
- 只有排空 pending operation 和结构化并发 scope 后才关闭。

timer 支持是可选的。实现 `schedule_after` 后可以使用事件循环的原生
timer；否则请使用明确的 timer sender 或应用自己的安全 fallback。

## 跨运行时通信

使用 `co::oneshot` 和 `co::mpsc` 在 worker 线程之间传递值。通道会
观察 stop token，因此取消可以协作传播而不阻塞任一 loop。

task 中调用 DartFn 时，bridge 会发送 Dart frame 并等待 reply sender。不要从
`BRIDGE_SYNC` 调用可能挂起的 DartFn。

## 关闭顺序

停止接收新任务，发起协作式取消，排空 async scope，停止外部 loop，最后销毁
scheduler。未完成的 stdexec operation 可能仍持有 scheduler 引用。

## 从 v1 迁移

| v1 | v2 |
| --- | --- |
| `ForeignExecutor` | 用户提供的 `stdexec::scheduler` |
| `dcb_foreign_register*` | 不再需要 bridge 注册 API |
| `foreign_runtime.h` | 应用自己的 scheduler 头文件 |
| `Signal / Slot` | `stop_token` |
| async-simple `Lazy` | `stdexec::task` |

完整 sender / scheduler 规则见 [stdexec 异步 C++ 指南](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/)
和[版本说明](/dart_cpp_bridge/zh-cn/versions/)。
