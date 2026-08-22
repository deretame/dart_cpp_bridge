---
title: 多个运行时
description: 使用 stdexec scheduler 运行独立 Asio 事件循环，并通过 bridge channel 通信
---

:::note[v2.1.0]
本页按已发布的 stdexec 运行时编写。旧的 async-simple 示例只保留在 v1
文档归档中。
:::

`dcb::Runtime` 是进程级单例。如果应用需要另一套事件循环，应为它自己持有
`io_context`、`dcb::IoContextScheduler` 和线程。这个 worker scheduler 就是
普通的 stdexec scheduler，不需要注册第二个 bridge runtime，也不再使用已删除
的 `AsioExecutor` API。

## Worker 结构

可直接运行的实现见 `examples/multi_runtime_demo/worker_runtime.hpp`。核心所有权
规则是：

- `io_context`、work guard、scheduler 和线程必须一起存活；
- 用 `stdexec::starts_on(worker.scheduler(), task)` 启动 worker task；
- 停止时只能从非 IO 上下文 join worker，因为 worker task 可能正在等待由主
  bridge IO 线程投递的 Dart 回复。

```cpp
class WorkerRuntime {
  DCB_ASIO_NS::io_context io_;
  std::shared_ptr<dcb::IoContextScheduler> scheduler_;
  std::unique_ptr<std::thread> thread_;

 public:
  dcb::IoContextScheduler& scheduler() { return *scheduler_; }

  template <class S>
  void spawn(S&& task) {
    exec::start_detached(
        stdexec::starts_on(*scheduler_, std::forward<S>(task))
        | stdexec::upon_error(log_error));
  }
};
```

## 跨运行时 channel

单次请求/回复使用 `co::oneshot`，连续值使用 `co::mpsc`。把状态作为参数传给
具名协程函数，让它由 coroutine frame 持有：

```cpp
using StringTx = co::oneshot::Sender<std::string>;

stdexec::task<void> process_on_worker(StringTx tx, std::string input) {
  tx.send("processed: " + input);
  co_return;
}

stdexec::task<std::string> process(WorkerRuntime& worker,
                                   std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  worker.spawn(process_on_worker(std::move(tx), std::move(input)));
  auto reply = co_await std::move(rx);
  if (!reply) throw std::runtime_error("worker dropped");
  co_return *reply;
}
```

producer 应等待 worker scheduler 的 timer，不要阻塞 worker 线程：

```cpp
stdexec::task<void> produce(dcb::IoContextScheduler scheduler,
                            co::mpsc::Sender<std::string> tx) {
  for (int i = 0; i != 5; ++i) {
    co_await scheduler.schedule_after(std::chrono::milliseconds(100));
    if (!tx.send("item_" + std::to_string(i))) co_return;
  }
}
```

## 在 worker 中调用 Dart 函数

`DartFn` 可以在 worker task 中等待。回复会按照 task 的 home scheduler 恢复，
因此回调后的代码仍然回到 worker loop：

```cpp
stdexec::task<void> call_dart_on_worker(
    StringTx tx, dcb::DartFn<std::string(std::string)> callback,
    std::string input) {
  try {
    tx.send(co_await callback(std::move(input)));
  } catch (const std::exception& e) {
    tx.send(std::string("ERROR: ") + e.what());
  }
  co_return;
}
```

不要在 `co_await` 时持有 worker mutex，不要阻塞 worker loop，也不要在 IO 线程
调用 `dcb::sync_wait`。完整的 codegen fixture 还覆盖 pipeline、fan-out、stream、
worker 停止和 DartFn 异常：`examples/multi_runtime_demo/`。

## 延伸阅读

- [内置运行时](/dart_cpp_bridge/zh-cn/guides/fundamentals/runtime/)
- [外部运行时集成](/dart_cpp_bridge/zh-cn/guides/advanced/foreign-runtime/)
- [v1 归档的多运行时指南](/dart_cpp_bridge/zh-cn/v1/guides/advanced/multi-runtime/)
