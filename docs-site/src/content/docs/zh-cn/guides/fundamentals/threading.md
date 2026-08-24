---
title: 线程与阻塞任务
description: Runtime 线程、spawn_blocking、线程池配置和自定义 scheduler
---

v2 Runtime 有两个内置执行上下文：

| 上下文 | 线程数 | 用途 |
|--------|--------|------|
| io scheduler | 默认一个，可配置 | bridge dispatch、定时器、非阻塞协程续接 |
| blocking scheduler | 可配置的 Asio 线程池 | BRIDGE_NORMAL 和默认 spawn_blocking |

io scheduler 默认只有一个线程，保持原来的单线程行为；需要更多事件循环
并发时可以在启动前配置线程数量。配置多个线程后，业务共享状态需要由应用
自己同步。

## 选择正确的入口

只有在操作极短且绝不阻塞时才使用 BRIDGE_SYNC。完整的同步阻塞操作使用
BRIDGE_NORMAL：

~~~cpp
BRIDGE_NORMAL
std::string read_file(std::string path) {
  return read_entire_file(path);  // 可以阻塞
}
~~~

生成的 Dart API 返回 Future<String>，native 函数则在内置 blocking scheduler
上执行。

如果阻塞只是更大异步流程中的一个阶段，则使用 BRIDGE_ASYNC +
spawn_blocking：

~~~cpp
BRIDGE_ASYNC
stdexec::task<Decoded> load_and_decode(std::string path) {
  auto bytes = co_await dcb::spawn_blocking(
      [path = std::move(path)] {
        return read_entire_file(path);
      });
  co_return co_await decode_async(std::move(bytes));
}
~~~

callable 在 blocking pool 执行；完成后 sender 会切回 bridge io scheduler，
task 可以继续执行而不会阻塞 io 线程。

## 配置 io scheduler

io scheduler 默认使用一个线程。必须在第一个 session 启动 Runtime 前配置：

~~~dart
await DcbLib.init(ioThreads: 2);
~~~

底层 API 使用同名参数：

~~~dart
await DartCppBridge.init(
  bindings: createBindings(),
  ioThreads: 2,
);
~~~

C++ 管理 Runtime 时，在 `Runtime::start()` 前调用
`Runtime::set_io_threads(2)`。传入 0 会规范化为 1；启动后修改会被忽略。
它配置的是共享 Asio `io_context` 的 runner 数量，不是每个 isolate 一个
scheduler。

增加 runner 数量不会让阻塞等待自动安全。原始
`stdexec::sync_wait` 会一直占用调用它的 runner，直到等待返回。2 个 runner
时，如果只有一个 runner 在等待，另一个空闲 runner 可以执行被等待的任务；
任务结束后，之前被占用的线程会回到 `io_context` 并继续接收工作。如果每个
runner 都在等待同一个 scheduler 上排队的任务，所有 runner 都被占用，整个
scheduler 仍然会死锁。

## 配置内置线程池

默认线程池大小为 4。必须在 Runtime 启动之前配置：

~~~dart
await DcbLib.init(threadPoolSize: 8);
~~~

使用底层 DartCppBridge API 时，参数名是 poolThreads：

~~~dart
await DartCppBridge.init(
  bindings: createBindings(),
  poolThreads: 8,
);
~~~

如果由 C++ 管理 Runtime，则在 Runtime::start 之前，或第一次打开 session
之前调用 Runtime::set_pool_threads。传入 0 会被规范化为 1。启动后再修改
不会影响已经创建的线程池。

这会全局修改内置 pool 的并发度，不会为每个函数创建新的线程池。

## 为单类工作使用自定义线程池

spawn_blocking 接受 scheduler 参数，可以把某类工作隔离到独立线程池：

~~~cpp
#include <exec/static_thread_pool.hpp>

namespace {
exec::static_thread_pool codec_pool{8};
}

BRIDGE_ASYNC
stdexec::task<int> decode_on_codec_pool(
    std::uint64_t address, std::int32_t length) {
  co_return co_await dcb::spawn_blocking(
      [address, length] {
        return decode_native_buffer(address, length);
      },
      codec_pool.get_scheduler());
}
~~~

自定义 scheduler 只作用于这次操作；BRIDGE_NORMAL 仍然使用内置 pool。
必须让 codec_pool 活到所有使用它的 sender 和 operation 完成。服务场景中，
建议把 pool 放进 service 对象，并在 service 关闭时停止和 join。

scheduler 可以是任何满足 stdexec scheduler contract 的类型，包括 Asio
adapter、static thread pool，或
[外部运行时示例](/dart_cpp_bridge/zh-cn/guides/advanced/foreign-runtime/)中的
foreign event-loop scheduler。

## sync_wait 的限制

dcb::sync_wait 适合非协程、且不在 io 线程上的调用者：

~~~cpp
auto result = dcb::sync_wait(
    stdexec::starts_on(
        *dcb::Runtime::instance().io_scheduler(),
        stdexec::just(42)));
~~~

它会阻塞调用线程直到完成，并返回 optional tuple。从任何 io scheduler runner
调用都会抛异常，不依赖“可能还有空闲线程”来规避死锁。BRIDGE_ASYNC task 中
应使用 co_await；BRIDGE_NORMAL 中可以使用 sync_wait，但仍要避免形成业务层
循环等待。

## 线程安全检查清单

- 把 bridge io 回调视为运行在配置的 scheduler runner 上；当 `ioThreads > 1`
  时，共享状态必须自行同步；
- 不要在 scheduler runner 调用 sleep_for、等待互斥锁、执行阻塞 I/O 或使用
  sync_wait；
- channel sender 可以从 pool 线程 send；mpsc receiver 仍然只能单消费者；
- 指针 buffer 必须存活到整个 native operation 完成，尤其是 operation 被
  移到线程池时；
- spawn_blocking 完成后可能回到 io_scheduler；其他线程也访问的业务状态需要
  自己加同步；
- 销毁 scheduler 前，先排空 async scope 并 join 自定义线程池。

## 相关 API

- [v2 中的 stdexec](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/)
- [内置 Runtime](/dart_cpp_bridge/zh-cn/guides/fundamentals/runtime/)
- [Channel 通道](/dart_cpp_bridge/zh-cn/guides/fundamentals/channels/)
- [大缓冲区指针映射](/dart_cpp_bridge/zh-cn/codegen/type-mapping/#大缓冲区地址传递模式)

