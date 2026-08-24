---
title: 内置运行时（v2）
description: 已发布 v2.1.0 使用的 Asio 与 stdexec 运行时
---

:::note[v2.1.0]
本页描述当前 stdexec 运行时。已发布的 v1 项目请查看
[v1 async-simple 归档](/dart_cpp_bridge/zh-cn/v1/guides/fundamentals/async-simple/)。
:::

`dart_cpp_bridge` 内置一个由 Asio 和 stdexec 组成的进程级 Runtime。
`DartCppBridge.init()` 会自动启动它，C++ 业务代码无需再创建一套事件循环。

## Runtime 组件

- **Asio `io_context`** — 默认一个 runner，负责 bridge dispatch 和非阻塞任务；
  runner 数量可在启动前配置；
- **`IoContextScheduler`** — v2 的 stdexec scheduler，由
  `Runtime::io_scheduler()` 返回；
- **Asio thread pool** — blocking scheduler，由
  `Runtime::blocking_scheduler()` 返回；
- **通道** — `co::oneshot` 和 `co::mpsc` sender，用于跨线程通信。

Runtime 头文件通过 `DCB_ASIO_NS` 暴露当前选择的 Asio 实现：默认展开为
`asio`，设置 `DCB_USE_BOOST_ASIO=ON` 后展开为 `boost::asio`。业务代码应
使用 `DCB_ASIO_NS::...`，这样同一份源码可以适配两种实现。宿主工程提供
stdexec/Asio 时的 CMake 配置见[依赖所有权与 Asio 命名空间](/dart_cpp_bridge/zh-cn/guides/fundamentals/native-assets-hooks/#cmake-依赖由谁负责)。

```cpp
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>

auto& runtime = dcb::Runtime::instance();
runtime.start();                         // 通常由 Dart init 完成
auto* io = runtime.io_scheduler();        // 默认一个 runner
auto blocking = runtime.blocking_scheduler();
```

## 异步业务代码

用 `stdexec::task<T>` 编写通过 `BRIDGE_ASYNC` 暴露的协程：

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
#include <string>

BRIDGE_ASYNC
stdexec::task<std::string> delayed_echo(std::string message) {
  co_await dcb::sleep(std::chrono::milliseconds(100));
  co_return message;
}
```

生成的 dispatch 会在 bridge scheduler 上启动 task。协程挂起时不会占用 io
线程。使用 `starts_on`、`continues_on`、`on`、`when_all`、`sync_wait` 等当前
stdexec 名称，不要在 v2 中使用 v1 的 `Lazy` / `Executor` API。

## 阻塞工作

不要阻塞 io 线程。需要保持在异步流水线中时，用 `dcb::spawn_blocking`：

```cpp
BRIDGE_ASYNC
stdexec::task<int> compute(int n) {
  auto value = co_await dcb::spawn_blocking([n] {
    return expensive_synchronous_work(n);
  });
  co_return value;
}
```

完全普通的阻塞函数使用 `BRIDGE_NORMAL`；生成的 dispatch 会把它放到
blocking pool 执行，但 Dart 侧仍得到 `Future<T>`。

## 在非协程函数中等待

`dcb::sync_wait(sender)` 是给非协程调用方使用的阻塞便利函数。它会拒绝在
任何 io scheduler runner 上调用，因为在那里等待会造成事件循环自死锁。即使
配置了多个 runner，原始 `stdexec::sync_wait` 也只是在还有空闲 runner 时可能
完成；它会占用调用线程，所有 runner 同时等待时仍会让 scheduler 死锁：

```cpp
auto result = dcb::sync_wait(
    stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                       stdexec::just(42)));
```

请从 worker 或外部线程调用，不要从 `BRIDGE_SYNC` 业务代码调用。

## 通道与取消

```cpp
auto [tx, rx] = dcb::co::oneshot::channel<std::string>();
tx.send("hello");

// 在 stdexec::task 中：
auto value = co_await std::move(rx);  // std::optional<std::string>
```

对于 mpsc channel，使用 `co_await rx.recv()`；oneshot receiver 本身就是
sender。`recv()` 会观察 stop token。v2 使用
`stdexec::inplace_stop_source` / `stdexec::stop_token` 做协作式取消，取消后
通过 `set_stopped()` 完成。Dart `Future` 不能被强制取消，需要时应由业务层
额外暴露 task ID 和 cancel 方法。

## Scheduler 配置

io scheduler 默认有 1 个 runner。必须在第一个 session 启动 Runtime 前通过
`DartCppBridge.init(ioThreads: 2)` 配置；C++ 可以在 `start()` 前调用
`Runtime::set_io_threads(2)`。传入 0 会规范化为 1，启动后修改会被忽略。

## 线程池配置

内置 blocking pool 默认有 4 个线程。必须在第一个 session 启动 Runtime 前
通过 `DartCppBridge.init(poolThreads: 8)`（或生成 API 的 `threadPoolSize`）
配置。C++ 可以在 `start()` 前调用 `Runtime::set_pool_threads(8)`。如果某类
工作需要独立线程池，把它的 scheduler 作为 `dcb::spawn_blocking` 的第二个
参数传入，详见[线程与阻塞任务](/dart_cpp_bridge/zh-cn/guides/fundamentals/threading/)。

## 线程规则

:::caution

- 不要在 io scheduler runner 执行阻塞 I/O、sleep 或阻塞锁；
- 不要从 io scheduler runner 调用 `dcb::sync_wait`；
- 传给 detached 或 scope 操作的完成 lambda 尽量标记 `noexcept`；
- 销毁 scheduler 前先排空结构化并发 scope。
:::

## 示例与参考

- `examples/base_demo` — Runtime 冒烟测试和 wire dispatch；
- `examples/multi_runtime_demo` — 独立 Runtime / channel 模式；
- `examples/foreign_runtime_demo` — 把 libuv 暴露为普通 stdexec scheduler；
- [v2 stdexec 异步 C++](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/)；
- [Channel 通道](/dart_cpp_bridge/zh-cn/guides/fundamentals/channels/)；
- [线程与阻塞任务](/dart_cpp_bridge/zh-cn/guides/fundamentals/threading/)；
- [架构](/dart_cpp_bridge/zh-cn/guides/fundamentals/architecture/)；
- [版本文档](/dart_cpp_bridge/zh-cn/versions/)。
