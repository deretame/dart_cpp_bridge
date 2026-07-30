---
title: async-simple 协程入门
description: dart_cpp_bridge 使用的 async-simple 协程库基础用法：Lazy、Executor、co_await、启动与线程模型
---

`dart_cpp_bridge` 的 C++ 异步层基于 [async-simple](https://alibaba.github.io/async_simple/) 构建。你不需要成为 async-simple 专家就能写业务代码，但这章会介绍“不读官方文档也能上手”的最小必要知识。

## 核心概念

async-simple 是一个 C++20 协程库，bridge 里主要用到两个概念：

- **`async_simple::coro::Lazy<T>`** — 一个懒启动的协程任务。函数返回 `Lazy<T>`，调用时不会立刻执行，只有被 `co_await`、`.start()` 或 `syncAwait()` 时才会启动。
- **`async_simple::Executor`** — 协程调度器。`dcb::AsioExecutor` 和 `dcb::ForeignExecutor` 都是它的实现。协程挂起后由 executor 决定在哪个线程上恢复。

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
- [基础运行时](/dart_cpp_bridge/guides/fundamentals/runtime/) — Runtime、spawn、channel、sleep
- [外部运行时集成](/dart_cpp_bridge/guides/advanced/foreign-runtime/) — ForeignExecutor、MSVC 注意事项
