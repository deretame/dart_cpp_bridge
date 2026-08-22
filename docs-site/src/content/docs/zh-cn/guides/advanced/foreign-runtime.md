---
title: 外部运行时集成（v2）
description: 让 libuv 或其他外部事件循环提供 stdexec scheduler
---

:::note[v2.1.0]
v2 不再提供 `ForeignExecutor` 或 `foreign_runtime.h`。外部事件循环应
通过普通 stdexec scheduler 接入。旧 v1 注册 API 只保留在 v1 源码归档中。
:::

## 选择接入方式

外部异步运行时有两种合法的接入方式，应根据外部库暴露的 API 选择合适的
层级：

1. **推荐：适配为 stdexec scheduler。** 如果你拥有事件循环，或者需要在
   该运行时上组合多个异步操作，这是更完整的方案。
2. **轻量：把回调接成 sender 管道。** 如果外部库只提供 callback API，且
   不想实现 scheduler，可以让外部运行时继续自己管理，通过 channel sender
   把数据接入 bridge 的管道。

### 推荐：暴露一个 scheduler

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

### 轻量方案：把回调接入 sender 管道

不需要把每一个外部运行时都适配成 scheduler。对于只有 callback API 的外部库，
可以按原方式启动操作，在回调中把完成结果发送到 `co::oneshot` 或 `co::mpsc`
channel，再用普通的 stdexec 组合子继续处理。外部库仍然拥有自己的事件循环；
channel 只是数据进入 bridge 管道的边界。

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <dart_cpp_bridge/channel.hpp>
#include <stdexec/execution.hpp>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// 外部库提供的 API；callback 可能在任意线程执行。
void external_fetch_async(
    std::string url,
    std::function<void(int, std::string)> callback);

BRIDGE_ASYNC
stdexec::task<std::string> fetch_with_callback(std::string url) {
  auto [tx, rx] = dcb::co::oneshot::channel<std::string>();
  auto callback_tx = std::make_shared<
      dcb::co::oneshot::Sender<std::string>>(std::move(tx));

  external_fetch_async(
      std::move(url),
      [callback_tx](int status, std::string body) {
        if (status == 0) {
          callback_tx->send(std::move(body));
        } else {
          callback_tx->send_error(std::make_exception_ptr(
              std::runtime_error("external request failed")));
        }
      });

  auto body = co_await (
      std::move(rx) |
      stdexec::then([](std::optional<std::string> value) {
        if (!value) {
          throw std::runtime_error("callback result was closed");
        }
        return std::move(*value);
      }) |
      stdexec::then([](std::string value) {
        // 在这里添加业务自己的结果转换。
        return value;
      }));
  co_return body;
}
```

多事件回调可以改用 `co::mpsc`，但应根据是否需要背压选择 channel 类型：

- `co::mpsc::unbounded`：`tx.send(value)` 是立即完成的非阻塞调用，返回
  `bool`，可以直接从 callback 调用；代价是缓冲区没有上限。
- `co::mpsc::bounded`：`tx.send(value)` 返回 sender/awaiter，因为容量不足时
  需要等待。它必须在 sender/task 上下文中被启动或 `co_await`；不要在 callback
  中调用后直接丢弃返回的 sender。如果 callback 无法 await，应把 send operation
  交给 task 或 scheduler，或者在这个边界使用 unbounded channel。

两种情况下，task 都通过 `co_await rx.recv()` 消费，直到生产者关闭 channel。
如果等待中的 operation 已销毁，迟到的 oneshot send 会返回 `false`；外部库仍
需要自行提供取消或生命周期策略，才能在需要时真正停止底层工作。

这种方式适合 C 库、SDK，以及公共 API 已经是回调形式的事件循环。它不会把外部
运行时变成 stdexec scheduler；如果需要在外部运行时本身进行调度、定时或结构化
组合，仍应使用上面的 scheduler 方案。

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
