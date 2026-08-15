---
title: v2 中的 stdexec
description: Sender、task、scheduler、取消和阻塞任务的实际使用方式
---

:::caution[v2 专用]
当前开发线使用 stdexec。已发布的 v1 项目使用 async-simple，API 保留在
[v1 文档](/dart_cpp_bridge/zh-cn/v1/) 中。v2 头文件不要再混入
`async_simple::coro::Lazy` 示例。
:::

## 先建立正确的模型

stdexec 把异步操作拆成三个值：

- **sender**：描述要执行什么的惰性配方；
- **scheduler**：描述从哪里开始或在哪个执行上下文继续；
- **receiver**：接收一次完成结果：value、error 或 stopped。

构造 sender 不会执行任何工作；只有 connect 并 start 后才会运行。因此
管道操作符表达的是流水线，不是立即调用：

```cpp
auto sender = stdexec::just(40) |
    stdexec::then([](int value) noexcept { return value + 2; });
// 此时还没有执行。
```

bridge 生成的 dispatch 会替你启动导出的操作。业务代码通常只需要编写
stdexec::task<T> 协程，并用 co_await 等待 sender：

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
#include <chrono>
#include <string>

BRIDGE_ASYNC
stdexec::task<std::string> delayed_echo(std::string message) {
  co_await dcb::sleep(std::chrono::milliseconds(100));
  co_return message;
}
```

stdexec::task<T> 是 v2 生成 API 使用的异步返回类型，并且自带 scheduler
affinity：bridge 在某个 scheduler 上启动后，协程会回到该 scheduler 恢复。
不要再定义 bridge 专用 Future 类型。

## 如何选择标记

| 标记 | 执行位置 | Dart 结果 | 适用场景 |
|------|----------|-----------|----------|
| BRIDGE_SYNC | bridge io 线程 | T | 极短且绝不阻塞的工作 |
| BRIDGE_ASYNC | bridge io 线程上的协程 | Future<T> | 定时器、通道、异步组合 |
| BRIDGE_NORMAL | 内置 blocking pool | Future<T> | 阻塞 I/O 或 CPU 密集的同步工作 |

BRIDGE_SYNC 和 BRIDGE_ASYNC 不能调用 sleep_for、等待互斥锁、执行阻塞文件
I/O 或调用 dcb::sync_wait。请改用 BRIDGE_NORMAL 或 dcb::spawn_blocking。

## 显式启动 sender

内置事件循环通过 Runtime::io_scheduler() 暴露。仓库使用的当前
stdexec 名称是 starts_on、continues_on、write_env 和 read_env：

```cpp
#include <dart_cpp_bridge/runtime.hpp>
#include <stdexec/execution.hpp>
#include <tuple>

auto sender = stdexec::just(40) |
    stdexec::then([](int value) noexcept { return value + 2; });

auto result = dcb::sync_wait(stdexec::starts_on(
    *dcb::Runtime::instance().io_scheduler(), std::move(sender)));

if (result) {
  int value = std::get<0>(*result);  // optional<tuple<...>> 的第一个值
}
```

dcb::sync_wait 会阻塞调用线程，并返回 stdexec 的结果形态
（optional<tuple<...>>）。它额外带有死锁保护：如果从 bridge io 线程调用
会抛异常。只能从 worker 或外部线程使用，不能从 BRIDGE_SYNC 或 io 线程回调
使用。

几个调度组合子的职责不同：

- starts_on(sched, sender)：选择操作的开始位置；
- continues_on(sender, sched)：把下游完成切换到 sched；
- on(sched, sender)：在 sched 执行操作，但完成后回到起始 scheduler；它不会
  把整个后续管道永久固定到目标 scheduler；
- stdexec::schedule(sched)：产生一个在 scheduler 上执行一次、无返回值的 sender。

生成 dispatch 通常已经提供 starts_on(io_scheduler, ...)，只有在自己编写
launcher 时才需要重复显式启动。

## 组合异步操作

在 task 中，单值 sender 的 co_await 结果就是该值本身：

```cpp
BRIDGE_ASYNC
stdexec::task<int> total() {
  auto left = co_await read_left();
  auto right = co_await read_right();
  co_return left + right;
}
```

独立操作可以用 when_all 并行等待。每个分支都返回一个值时，结果会扁平化，
可以直接结构化绑定：

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> parallel_greeting() {
  auto [name, suffix] = co_await stdexec::when_all(
      load_name(), load_suffix());
  co_return name + suffix;
}
```

常用组合子：

| 工具 | 含义 |
|------|------|
| then | 把成功值映射成新值 |
| let_value | 根据上一步的值动态返回新的 sender |
| when_all | 等待所有分支；失败时向兄弟分支请求停止 |
| exec::when_any | 第一个完成者胜出；其余分支收到停止请求 |
| upon_error | 转换或记录错误 |
| upon_stopped | 为 stopped 提供兜底 |

then 回调抛出的异常会变成 set_error，最终由生成的 dispatch 编码为 Dart
错误。

## 阻塞任务和自定义 scheduler

dcb::spawn_blocking 用于把同步 callable 放进异步流水线：

```cpp
#include <dart_cpp_bridge/runtime.hpp>

BRIDGE_ASYNC
stdexec::task<int> compute(int n) {
  auto value = co_await dcb::spawn_blocking([n] {
    return expensive_synchronous_work(n);
  });
  co_return value;
}
```

callable 会在 Runtime 的 blocking pool 执行，完成后切回 bridge io scheduler。
默认线程池大小是 4。

可以在 Runtime 启动前配置内置线程池：

```dart
await DcbLib.init(threadPoolSize: 8);
```

如果由 C++ 管理 Runtime，则在第一次 start() 或打开 session 之前调用
Runtime::set_pool_threads(8)。生成 Dart API 的选项名是 poolThreads，也必须
在第一个 isolate 启动 Runtime 之前设置。

spawn_blocking 还接受任意 stdexec scheduler，可以为某类工作使用独立线程池：

```cpp
#include <exec/static_thread_pool.hpp>

namespace {
// 必须活到所有使用它的操作完成。
exec::static_thread_pool image_pool{8};
}

BRIDGE_ASYNC
stdexec::task<int> decode_image(std::uint64_t address, std::int32_t len) {
  auto pixels = co_await dcb::spawn_blocking(
      [address, len] { return decode_pixels(address, len); },
      image_pool.get_scheduler());
  co_return pixels;
}
```

BRIDGE_NORMAL 始终使用 Runtime 的内置线程池。如果某个操作需要自定义线程池，
请使用 BRIDGE_ASYNC + spawn_blocking(callable, custom_scheduler)。自定义
scheduler 必须比 sender 和从它启动的所有 operation 存活更久。

## 定时器

dcb::sleep 使用 Runtime 的定时 io scheduler，不会阻塞线程：

```cpp
BRIDGE_ASYNC
stdexec::task<void> wait_a_bit() {
  co_await dcb::sleep(std::chrono::milliseconds(50));
  co_return;
}
```

也可以传入实现 schedule_after(duration) 的自定义 timed scheduler；仓库的
examples/foreign_runtime_demo 展示了 libuv 版本。

## 取消和 environment

取消是协作式的。stop request 不会强杀 C++ 函数；操作必须观察 token，
自行结束或传播 set_stopped。使用当前的 write_env API 注入 token：

```cpp
stdexec::inplace_stop_source source;

auto cancellable = stdexec::write_env(
    some_sender(),
    stdexec::prop{
        stdexec::get_stop_token,
        source.get_token()});

source.request_stop();
```

channel、定时器和 spawn_blocking 都参与 sender 取消。Dart Future 本身不能
强制取消；如果需要从 Dart 取消，应额外暴露业务 operation ID 和 cancel 方法。
详见[Channels](/dart_cpp_bridge/zh-cn/guides/fundamentals/channels/)中的
可取消 send/receive 规则。

## Detached 工作和协程生命周期

故意不等待的后台任务需要 detached launcher 或 scope，并且 detached operation
必须自己拥有所需状态：

```cpp
template <class S>
void launch_on_io(S sender) {
  exec::start_detached(
      stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
          std::move(sender)) |
      stdexec::upon_error([](std::exception_ptr error) noexcept {
        log_error(error);
      }));
}
```

长生命周期服务优先使用结构化并发（exec::async_scope 或 counting scope），
并在销毁 scheduler 前排空 scope。exec::start_detached 和 async_scope::spawn
不会自动把未处理的错误变成 Dart response；请附加 upon_error，或在 task 内
处理异常。

生成的异步 dispatch 使用零捕获协程 IIFE，不要复制成带悬空捕获的形式：

```cpp
auto task = [](Request request) -> stdexec::task<void> {
  // 状态作为参数进入协程帧。
  co_await handle(std::move(request));
}(std::move(request));
```

## 仓库里的权威参考

本页负责说明实际使用路径。关于 completion signatures、connect/start、
async_scope、bulk、回调互操作和常见编译错误，请继续阅读仓库中经过编译验证的
参考文档：
[docs/cpp26_executor_model_usage.md](https://github.com/deretame/dart_cpp_bridge/blob/main/docs/cpp26_executor_model_usage.md)。

