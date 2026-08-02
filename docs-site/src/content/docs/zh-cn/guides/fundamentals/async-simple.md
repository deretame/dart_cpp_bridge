---
title: async-simple 协程入门
description: dart_cpp_bridge 使用的 async-simple 协程库基础用法：Lazy、Executor、co_await、启动与线程模型
---

`dart_cpp_bridge` 的 C++ 异步层基于 [async-simple](https://alibaba.github.io/async_simple/) 构建。你不需要成为 async-simple 专家就能写业务代码，但这章会介绍“不读官方文档也能上手”的最小必要知识。

## 核心概念

async-simple 是一个 C++20 协程库，bridge 里主要用到两个概念：

- **`async_simple::coro::Lazy<T>`** — 一个懒启动的协程任务。函数返回 `Lazy<T>`，调用时不会立刻执行，只有被 `co_await`、`.start()` 或 `syncAwait()` 时才会启动。
- **`async_simple::Executor`** — 协程调度器。`dcb::AsioExecutor` 和 `dcb::ForeignExecutor` 都是它的实现。协程挂起后由 executor 决定在哪个线程上恢复。

## 不要使用 uthread（改用 Boost.Fiber）

async-simple 还自带一个可选的栈式纤程实现 **uthread**。`dart_cpp_bridge` 刻意**不编译也不链接**它：

- runtime 只使用 async-simple 的头文件部分（`Lazy`、`Executor`、`Promise`/`Future`、`Signal`），没有任何 bridge 代码引用 uthread；
- uthread 的 static/shared 目标在 CMake 里被 `EXCLUDE_FROM_ALL` 排除（见 `dart/native/CMakeLists.txt`）；
- uthread 不支持 Windows；
- 它的 Darwin 汇编按 `CMAKE_SYSTEM_PROCESSOR` 选择，不认 `CMAKE_OSX_ARCHITECTURES`，macOS 跨架构 slice 会编译失败。

业务代码需要纤程时，请直接使用专门的纤程库，例如 **Boost.Fiber**。不要 include `async_simple/uthread/*` 头文件，也不要链接 `async_simple` / `async_simple_static`。

## 写一个协程

业务 C++ 函数只要返回 `Lazy<T>`，用 `co_await` 等待其他异步操作，用 `co_return` 返回结果：

```cpp
#include "async_simple/coro/Lazy.h"

async_simple::coro::Lazy<std::string> greet(std::string name) {
  // co_await 其他 Lazy、channel、sleep 等
  co_return "Hello, " + name;
}
```

:::caution

- 不要在 `Lazy<T>` 函数里用普通 `return`，必须写 `co_return`
- 不要把 `Lazy` 函数当作同步函数直接调用：`greet("world")` 只返回一个还没执行的任务
  :::

## 启动协程

### 绑定 executor 启动（推荐）

```cpp
auto* ex = dcb::Runtime::instance().executor();

my_coroutine(args)
    .via(ex)                              // 指定调度器
    .start([](async_simple::Try<T>&& t) {  // 异步回调
      if (t.hasError()) {
        // 处理异常
      } else {
        // 使用 t.value()
      }
    });
```

`.via(ex)` 会返回 `RescheduleLazy<T>`：它不会立即执行，而是把任务投递到 executor 上，稍后由 executor 线程运行。

### 从非协程上下文同步等待

```cpp
#include "async_simple/coro/SyncAwait.h"

auto result = async_simple::coro::syncAwait(
    dcb::spawn(my_coroutine()));
```

`dcb::spawn(...)` 会把 `Lazy` 绑定到 Runtime 的 executor 并返回 `RescheduleLazy<T>`。`syncAwait` 会阻塞当前线程直到结果返回。

:::danger
`syncAwait` 不能在 io 线程调用，否则被等待的协程也需要 io 线程恢复，会**自死锁**。
:::

### 启动后丢弃结果

```cpp
dcb::spawn_detached(my_coroutine());
```

等价于 `.via(ex).start([](auto&&){})`，但更安全。不要用 async-simple 原生的 `RescheduleLazy::detach()`，它会在 io 线程上重新抛出异常，导致事件循环崩溃。

## Executor 会沿 co_await 链传递

这是最常见的问题。答案：**会的，但不等于“所有嵌套协程都自动跑在 ex 上”**。

```cpp
async_simple::coro::Lazy<std::string> outer() {
  auto a = co_await inner_a();            // inner_a 会继承当前 executor
  auto b = co_await co::oneshot::recv();  // channel 恢复时也会调度回当前 executor
  co_return a + b;
}

// 启动时绑定 ex
outer().via(ex).start([](auto&&) {});
```

async-simple 的 `co_await` 会把当前 executor 传给被等待对象的 `coAwait(Executor*)` 方法。`Lazy` 和 bridge 的 `channel` 都实现了这个方法，所以它们会自动使用同一个 executor。

但如果你手动写：

```cpp
inner_a().start([](auto&&){});  // 没有 co_await，不继承 executor
```

那就另当别论了。

## 常见等待对象

### 另一个 Lazy

```cpp
async_simple::coro::Lazy<int> inner();

async_simple::coro::Lazy<int> outer() {
  auto v = co_await inner();  // 继承 executor
  co_return v + 1;
}
```

### Promise / Future

如果你需要在协程里**非阻塞地等待一个外部回调或另一个线程返回的数据**，并且希望异常能自动抛回协程，可以直接使用 `async_simple::Promise<T>` + `async_simple::Future<T>`。

基本模式：

```cpp
async_simple::coro::Lazy<std::string> wait_for_callback() {
  async_simple::Promise<std::string> p;
  auto fut = p.getFuture();

  // 把 promise 或回调传给外部系统
  register_callback([p = std::move(p)](std::string result) mutable {
    p.setValue(std::move(result));
  });

  // 协程挂起，直到 setValue / setException 被调用
  auto value = co_await std::move(fut);
  co_return value;
}
```

关键点：

- `co_await Future<T>` 会把当前协程挂起，**不占用线程**
- 外部系统（回调、线程、事件循环）完成后调用 `p.setValue(...)` 恢复协程
- 如果外部系统出错，调用 `p.setException(std::current_exception())`，异常会在 `co_await` 处重新抛出
- `Future<T>` 只支持单消费者、一次性使用

### 在线程里返回结果

这和 `dcb::spawn_blocking` 底层用到的机制一样。你也可以自己写：

```cpp
async_simple::coro::Lazy<int> fetch_from_thread() {
  async_simple::Promise<int> p;
  auto fut = p.getFuture();

  std::thread([p = std::move(p)]() mutable {
    try {
      int value = do_some_blocking_work();
      p.setValue(value);
    } catch (...) {
      p.setException(std::current_exception());
    }
  }).detach();

  co_return co_await std::move(fut);
}
```

### 桥接回调式 API

这是纯 C bridge (`cbridge_wait.hpp`) 的核心思路：

```cpp
// 示意：你的代码里自己维护一个 op_id → Promise 的映射
async_simple::coro::Lazy<std::vector<uint8_t>> wait_for_c_callback(
    uint64_t op_id) {
  auto fut = take_promise_for_op(op_id);  // 从你自己的 registry 取出对应 Promise 的 Future
  co_return co_await std::move(fut);
}
```

外部 C 代码不需要知道协程，只需要拿到结果后调用 `setValue` 或 `setException`。

:::note
这里的 `take_promise_for_op` 不是 bridge 提供的 API，而是你自己维护的映射表逻辑。实际用法可以参考 `cbridge_wait.hpp` 的实现思路。
:::

### void 返回值

如果结果不需要返回值，用 `async_simple::Unit` 而不是 `void`。原因和 `spawn_blocking` 一样：`Future<void>` 在 `co_await` 后不会调用 `Future::value()`，通过 `setException` 设置的异常会被静默吞掉；`Future<Unit>` 会走 `value()`，异常一定会重新抛出。

```cpp
async_simple::coro::Lazy<> wait_for_event() {
  async_simple::Promise<async_simple::Unit> p;
  auto fut = p.getFuture();

  register_callback([p = std::move(p)]() mutable {
    if (ok) {
      p.setValue(async_simple::Unit{});
    } else {
      p.setException(std::make_exception_ptr(std::runtime_error("failed")));
    }
  });

  co_await std::move(fut);
  co_return;
}
```

### 与 spawn_blocking 的关系

`dcb::spawn_blocking` 本质上就是：

```cpp
async_simple::Promise<WireT> p;
auto fut = p.getFuture();
asio::post(rt.pool(), [f = std::forward<F>(f), p = std::move(p)]() mutable {
  try { p.setValue(f()); } catch (...) { p.setException(std::current_exception()); }
});
co_return co_await std::move(fut);
```

所以如果你只是想把一个可调用对象扔到线程池并 `co_await` 结果，直接用 `spawn_blocking` 即可；`Promise/Future` 适合更灵活的场景（比如你自己管理的线程、外部事件循环、C 回调等）。

### 异步 sleep

```cpp
#include "async_simple/coro/Sleep.h"

async_simple::coro::Lazy<> delayed() {
  co_await async_simple::coro::sleep(std::chrono::milliseconds(100));
  co_return;
}
```

在 `AsioExecutor` 上，`sleep` 使用 `asio::steady_timer`，不占用线程。

## 取消（Signal & Slot）

通过 bridge 调用生成的 Dart `Future`，对应的是一个正在运行的 `Lazy`
协程。Dart 侧没有任何句柄可以“杀掉”这个协程，所以 **bridge 不支持直接取消
Future**。async-simple 的取消是协作式的：任务通过自己的 `async_simple::Slot`
监听一个 `async_simple::Signal`，需要取消时向信号发出 `SignalType::Terminate`。

### 用 id 取消异步操作

Dart 的 `Future` 没有 C++ 侧可以查找的标识，所以需要自己维护一个全局的
`task_id → Signal` 注册表，再按 id 取消：

```cpp
std::mutex g_mu;
std::unordered_map<std::string, std::shared_ptr<async_simple::Signal>> g_signals;

// 任意线程都可以调用（Signal::emits 是线程安全的）。
bool cancel_task(std::string id) {
  std::shared_ptr<async_simple::Signal> signal;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_signals.find(id);
    if (it == g_signals.end()) return false;
    signal = it->second;
  }
  return signal->emits(async_simple::SignalType::Terminate) !=
         async_simple::SignalType::None;
}
```

协程启动时把自己的信号放进 map，并用 `Lazy::setLazyLocal` 把它绑定到当前
协程链上，这样链上每个 `async_simple::coro::sleep()` 都会继承到 Slot，可以被
打断。异常会被 wire 边界捕获，Dart 侧看到的是 `StateError`：

```cpp
async_simple::coro::Lazy<std::string> cancellable_task(std::string id) {
  auto signal = async_simple::Signal::create();
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_signals[id] = signal;
  }
  // RAII guard：协程退出（正常 / 取消 / 失败）时 g_signals.erase(id)
  co_return co_await cancellable_task_impl(id).setLazyLocal(signal.get());
}

async_simple::coro::Lazy<std::string> cancellable_task_impl(std::string id) {
  try {
    co_await async_simple::coro::sleep(std::chrono::seconds(30));
  } catch (const async_simple::SignalException&) {
    throw async_simple::SignalException(
        async_simple::SignalType::Terminate, "task cancelled: " + id);
  }
  co_return "done:" + id;
}
```

### 库已经支持可取消的 sleep

`async_simple::coro::sleep()` 会调用 executor 的 `schedule(Func, Duration,
Slot*)`。`dcb::AsioExecutor` 已经实现了这个重载：当协程链绑定了取消信号
（通过 `setLazyLocal`，或者在 `collectAll<Terminate>` /
`collectAny<Terminate>` 里自动绑定）时，executor 会注册一个
`SignalType::Terminate` 处理器来取消底层的 `asio::steady_timer`，sleep 的
awaiter 会立刻抛 `SignalException`，而不是等满整个时长。
`examples/codegen_demo` 的 `cancellable_task` 就是这么取消的；集成测试
C01-C06 覆盖了正常完成、按 id 取消、及时打断等待、id 隔离和未知 id。

如果自己写 awaiter（自定义 IO），仍然可以用文档里的“自定义 awaiter”模式
（`signalHelper{Terminate}.tryEmplace` / `checkHasCanceled`）；executor 的修复
覆盖的是 `sleep()` / `after()`。

`ForeignExecutor`（libuv 等非 asio 事件循环）在运行时注册了可选定时器回调
（`dcb_foreign_register_ex`）时使用 loop 自己的定时器，否则回退到等待线程；
两条路径下可取消 sleep 都生效。详见
[外部运行时集成](/dart_cpp_bridge/guides/advanced/foreign-runtime/)。

完整的模型（`collectAny` / `collectAll` 结构化取消、`Lazy::setLazyLocal`、
`CurrentSlot`、`ForbidSignal`、自定义 awaiter）见官方文档：
[Signal and Cancellation](https://alibaba.github.io/async_simple/docs.en/SignalAndCancellation.html)。

## 一次运行多个任务（collectAll / collectAny）

当一个 bridge 调用里需要同时跑多个协程时，直接用 `collectAll` /
`collectAny` 家族，不要自己手写计数器和 Promise。

### collectAll — 等所有任务完成

`collectAll` 会启动所有任务，等到全部完成后一起返回。结果以 `Try<T>`
形式给出，所以**某个任务抛异常不会让 `co_await collectAll(...)` 直接抛出**，
每个错误都会被装进对应的 `Try` 里：

```cpp
// 变参形式：Lazy<T1>, Lazy<T2>, ... → tuple<Try<T1>, Try<T2>, ...>
auto [a, b] = co_await async_simple::coro::collectAll(
    compute_int(), compute_string());

// 容器形式：std::vector<Lazy<T>> → std::vector<Try<T>>
std::vector<async_simple::coro::Lazy<int>> input;
input.push_back(compute_int(1));
input.push_back(compute_int(2));
auto results = co_await async_simple::coro::collectAll(std::move(input));
```

`collectAllPara` 接口一样，但会把任务提交到当前 executor，让它们的挂起可以
交错。注意 bridge 的 `AsioExecutor` 是单线程的，所以这里的 "Para" 是 io
线程上的协作式交错，不是多核并行。

### collectAny — 返回第一个完成的任务

`collectAny` 启动所有任务，只要有一个完成就立刻返回，其余任务的结果会被
忽略。最经典的用法是做超时：把真正的工作和
`async_simple::coro::sleep(...)` 放在一起赛跑，谁先完成听谁的。

```cpp
auto res = co_await async_simple::coro::collectAny(
    slow_http_fetch(), quick_cache_lookup());
if (res.index() == 0) { /* 第一个参数赢了 */ }
```

### 取消输家：collectAll<Terminate> / collectAny<Terminate>

这两个函数都接受 `SignalType` 模板参数。当第一个任务完成时，这个信号会转发
给其余任务：

- `collectAny<SignalType::Terminate>` 立刻返回，输家会收到取消信号。任务必须
  配合：使用 `async_simple::coro::sleep(...)`（或其他检查 Slot 的 awaitable）
  的子任务会自动以 `SignalException` 退出；自定义 IO 则通过
  `co_await CurrentSlot{}` 和 `signalHelper` 配合（见上面的“取消”一节）。
- `collectAll<SignalType::Terminate>` 也会在第一个任务完成时给其余任务发
  信号，但**仍然会等所有任务结束**；被取消的任务在结果里表现为 `Try` 错误。

`examples/codegen_demo` 在 `collect_all_demo`、`collect_all_para_demo`、
`collect_all_error_demo`、`collect_all_cancel_demo`、`collect_any_demo` 和
`collect_any_cancel_demo` 里演示了这两种 API；集成测试 D01-D06 覆盖了结果
收集、`Try` 错误捕获和输家取消。

`collectAllPara`、`collectAllWindowed` 以及 `collectAny` 的回调形式见官方文档：
[Lazy / Collect（英文）](https://alibaba.github.io/async_simple/docs.en/Lazy.html)
或 [Lazy / Collect（中文）](https://alibaba.github.io/async_simple/docs.cn/Lazy.html)。

## 阻塞操作怎么办

永远不要直接在 io 协程里做阻塞 IO 或长时间计算。用 `dcb::spawn_blocking`：

```cpp
async_simple::coro::Lazy<int> compute() {
  auto result = co_await dcb::spawn_blocking([] {
    // 在线程池执行，可以阻塞
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 42;
  });
  co_return result;
}
```

## MSVC 协程 lambda 捕获 bug

在 MSVC 上，**不要在协程 lambda 里捕获变量**（如 `std::string`、`DartFn`、`shared_ptr`），恢复后捕获值会变成垃圾，导致 `ACCESS_VIOLATION`。

```cpp
// ✗ 崩溃
auto bad = [cb, input]() -> async_simple::coro::Lazy<> {
  co_await cb(input);
};

// ✓ 正确
static async_simple::coro::Lazy<> good(
    dcb::DartFn<std::string(std::string)> cb,
    std::string input) {
  co_await cb(input);
}
```

这个 bug 和 executor 无关，任何 executor 上的协程 lambda 都可能触发。详见 [MSVC 注意事项](/dart_cpp_bridge/guides/advanced/foreign-runtime/#msvc-注意事项)。

## 常见错误

| 错误                             | 原因                                                   |
| -------------------------------- | ------------------------------------------------------ |
| 在 io 线程调用 `syncAwait`       | 会死锁，必须在非 io 线程使用                           |
| 协程里阻塞 io 线程               | 会卡死整个事件循环                                     |
| 用 `RescheduleLazy::detach()`    | 异常会抛到 io 线程，导致崩溃；用 `dcb::spawn_detached` |
| 在协程 lambda 里捕获变量（MSVC） | 恢复后捕获值损坏，崩溃                                 |
| 把 `Lazy<T>` 函数当普通函数调用  | 只创建任务，不会执行                                   |

## 延伸阅读

- [async-simple 官方仓库](https://github.com/alibaba/async_simple)
- [Signal and Cancellation（官方文档）](https://alibaba.github.io/async_simple/docs.en/SignalAndCancellation.html) — Signal/Slot、结构化取消、自定义 awaiter
- [Lazy / Collect（官方文档）](https://alibaba.github.io/async_simple/docs.cn/Lazy.html) — collectAll / collectAny / collectAllPara / collectAllWindowed
- [基础运行时](/dart_cpp_bridge/guides/fundamentals/runtime/) — Runtime、spawn、channel、sleep
- [外部运行时集成](/dart_cpp_bridge/guides/advanced/foreign-runtime/) — ForeignExecutor、MSVC 注意事项
