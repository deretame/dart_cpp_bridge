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

### channel

```cpp
auto [tx, rx] = co::oneshot::channel<std::string>();
tx.send("hello");

auto value = co_await rx.recv();  // 挂起，收到后恢复
```

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
