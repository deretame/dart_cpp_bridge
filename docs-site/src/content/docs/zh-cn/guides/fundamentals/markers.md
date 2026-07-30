---
title: 标记选择指南
description: 如何选择 BRIDGE_SYNC / BRIDGE_ASYNC / BRIDGE_NORMAL / stream / DartFn
---

bridge 用 `BRIDGE_*` 标记决定 C++ 函数以什么方式暴露给 Dart。选错会导致阻塞、死锁或性能问题。这页是一张速查表 + 决策流程。

## 总览

| 标记 | 执行线程 | C++ 返回类型 | Dart 返回类型 | 能否阻塞 | 能否调 DartFn |
|---|---|---|---|---|---|
| `BRIDGE_SYNC` | io_context | `T` | `T` | ❌ | ❌（死锁） |
| `BRIDGE_ASYNC` | io_context 协程 | `async_simple::coro::Lazy<T>` | `Future<T>` | ❌ | ✅（co_await） |
| `BRIDGE_NORMAL` | thread_pool | `T` | `Future<T>` | ✅ | ✅（syncAwait） |
| Stream | io_context | `void` + `StreamSink<T>` | `Stream<T>` | ❌ | 取决于内部实现 |

核心原则：

- **永远不要阻塞 io_context 线程**
- **DartFn 不能和 `BRIDGE_SYNC` 一起用**

## 1. BRIDGE_SYNC — 同步调用

C++ 函数在 bridge 的 io 线程上同步执行，结果立即返回 Dart。

```cpp
BRIDGE_SYNC
std::int32_t bridge_version() { return 42; }
```

适合：

- 纯计算、getter、常量读取
- 微秒级操作（通常 < 1 μs）
- 不访问文件、网络、锁、sleep

不适合：

- 阻塞调用
- 调用 DartFn（会永久死锁，因为 Dart 回复需要 io 线程）

## 2. BRIDGE_ASYNC — 异步协程

C++ 函数返回 `async_simple::coro::Lazy<T>`，在 io 线程上以协程方式执行，遇到 `co_await` 会挂起，不占用线程。

```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b) {
  co_return a + b;
}

BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> fetch_url(std::string url) {
  // 可以 co_await channel、sleep、DartFn、spawn_blocking
  auto result = co_await co::oneshot::recv();
  co_return result;
}
```

适合：

- 异步 IO
- 需要等待其他协程 / channel / Dart 回调
- 需要组合多个异步操作

不适合：

- 阻塞操作（用 `BRIDGE_NORMAL` 或 `spawn_blocking`）

## 3. BRIDGE_NORMAL — 普通函数

C++ 函数是普通函数（不返回 `Lazy`），bridge 自动把它投递到 `thread_pool` 执行。Dart 侧仍然是 `Future<T>`。

```cpp
BRIDGE_NORMAL
std::string sleep_greeting(std::string name) {
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  return "Hello, " + name;
}
```

适合：

- 文件 IO、网络 IO 同步库
- CPU 密集型计算
- 任何会阻塞或耗时 > 微秒级的操作

注意：

- 函数内部可以阻塞，因为它跑在线程池，不是 io 线程
- 仍然可以通过 `async_simple::coro::syncAwait(dcb::spawn(...))` 调用 DartFn

## 4. Stream — 流

Stream 函数在 io 线程上运行，通过 `StreamSink<T>` 向 Dart 推送数据。

```cpp
BRIDGE_ASYNC
void ticks(dcb::StreamSink<std::int32_t> sink, std::int32_t count,
           std::int32_t interval_ms) {
  for (int i = 0; i < count; ++i) {
    sink.add(i);
    co_await async_simple::coro::sleep(std::chrono::milliseconds(interval_ms));
  }
  sink.end();
}
```

注意：

- 取消订阅只停止 Dart 侧接收，C++ 侧继续运行
- 取消后的 `add()` 会被静默丢弃

## 5. DartFn — 反向调用 Dart 闭包

`dcb::DartFn<Ret(Args...)>` 表示一个 Dart 闭包。它本身不是函数标记，而是参数类型，需要和 `BRIDGE_ASYNC` 或 `BRIDGE_NORMAL` 搭配。

### 异步调用（推荐）

在 `BRIDGE_ASYNC` 协程里 `co_await` 调用，io 线程真挂起。

```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> greet(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  auto reply = co_await callback(name);
  co_return "Dart said: " + reply;
}
```

### 持久化回调

用 `BRIDGE_PERSIST` 标记，闭包不会被调用结束后自动注销，可存储起来反复调用。

```cpp
BRIDGE_SYNC
BRIDGE_PERSIST
bool register_callback(dcb::DartFn<std::string(std::string)> callback);

BRIDGE_NORMAL
std::string invoke_callback(std::string name);
```

### 禁止

```cpp
// ❌ 死锁
BRIDGE_SYNC
std::string bad(dcb::DartFn<std::string(std::string)> callback);
```

## 决策流程

```text
需要返回 Stream 吗？
  → 是：用 StreamSink<T> 的 BRIDGE_ASYNC

需要调用 Dart 闭包吗？
  → 是：BRIDGE_ASYNC + DartFn（co_await）
  → 或 BRIDGE_NORMAL + syncAwait(dcb::spawn(fn(args)))

函数会阻塞 / 耗时 / 文件 IO 吗？
  → 是：BRIDGE_NORMAL

函数是异步的，需要 co_await / channel / sleep？
  → 是：BRIDGE_ASYNC

只是纯计算 / getter / 微秒级操作？
  → 是：BRIDGE_SYNC
```

## 常见错误

| 错误 | 后果 |
|---|---|
| `BRIDGE_SYNC` 里阻塞 | io 线程卡住，整个 bridge 无响应 |
| `BRIDGE_SYNC` + DartFn | 永久死锁 |
| `BRIDGE_ASYNC` 里阻塞 | 同 `BRIDGE_SYNC` 阻塞 |
| 在 `BRIDGE_NORMAL` 里写 `co_await` | 编译错误，因为它不是协程 |

## 延伸阅读

- [async-simple 协程入门](/dart_cpp_bridge/guides/fundamentals/async-simple/)
- [基础运行时](/dart_cpp_bridge/guides/fundamentals/runtime/)
- [异常与错误处理](/dart_cpp_bridge/guides/fundamentals/errors/)
