---
title: Stream 流
description: 用 StreamSink 向 Dart 暴露 C++ 数据流，包括可选 stream
---

:::note[v2.1.0]
异步 stream 示例使用 `stdexec::task` 和 `dcb::sleep`。stream wire 契约与 v1
共用，变化仅在 C++ 异步签名。
:::

## 概述

带导出标记（`BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL`）**且**带必需
`dcb::StreamSink<T>` 参数的 C++ 函数会生成 Dart `Stream<T>`。导出标记是门槛：
只有 `StreamSink` 参数而没有导出标记的函数不会生成（生成器会告警并跳过）。
普通 `void` stream 函数通常用 `BRIDGE_NORMAL`；sink 参数会覆盖该标记的常规调度语义，
生成的 Dart API 返回 `Stream<T>`。

| C++ | Dart |
|---|---|
| `void f(dcb::StreamSink<T> sink, ...)` | `Stream<T> f(...)` |
| `R f(args..., std::optional<dcb::StreamSink<T>> progress)`（`BRIDGE_SYNC`） | `R f(..., {StreamController<T>? progress})` |
| `stdexec::task<R> f(args..., std::optional<dcb::StreamSink<T>> progress)` | `Future<R> f(..., {StreamController<T>? progress})` |

函数可以立刻返回：sink 可以被长期持有，之后在任意线程继续使用。
本页示例以 [codegen_demo](https://github.com/deretame/dart_cpp_bridge/tree/main/examples/codegen_demo) fixture 为准。

## 必需 stream

### C++ 头文件

```cpp
BRIDGE_NORMAL
void tick_stream(dcb::StreamSink<std::int32_t> sink, std::int32_t count = 5,
                 std::int32_t interval_ms = 10);
```

### C++ 实现

这是普通 `void` 函数，通常立刻返回，真正的工作在其他线程继续（这里是运行时线程池）；
`sink.add()` 是线程安全的。

```cpp
void tick_stream(dcb::StreamSink<std::int32_t> sink, std::int32_t count,
                 std::int32_t interval_ms) {
  asio::post(dcb::Runtime::instance().pool(),
             [sink = std::move(sink), count, interval_ms]() mutable {
               for (std::int32_t i = 0; i < count; ++i) {
                 sink.add(i);
                 if (interval_ms > 0) {
                   std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
                 }
               }
               sink.end();
             });
}
```

### 生成的 Dart

```dart
Stream<int> tickStream({int count = 5, int intervalMs = 10}) => ...;
```

### Dart 用法

```dart
// 收集全部事件：
final values = await tickStream(count: 5, intervalMs: 10).toList();
// values == [0, 1, 2, 3, 4]

// 或者 listen 后取消：
final sub = tickStream(count: 100, intervalMs: 10).listen((v) { ... });
await sub.cancel();
```

## 可选 stream

`std::optional<dcb::StreamSink<T>>` 会让 Dart 侧签名从「返回 stream」变成「接收 stream」：
调用方传入 `StreamController<T>?` 输入参数。传 `null` 时 C++ 收到 `std::nullopt`，
不产生任何流事件，函数仍正常返回结果。它可用于 `BRIDGE_SYNC`、`BRIDGE_ASYNC`、
`BRIDGE_NORMAL` 函数。对 `BRIDGE_SYNC`，C++ 在阻塞 FFI 调用期间发出的事件会排队到
reply port，调用返回后立即送达 controller。

### C++ 头文件

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress);
```

### C++ 实现

协程运行在 io 线程上：事件应该作为异步工作的一部分发出（在 `co_await` 之间），
绝不能在 io 线程上阻塞。

```cpp
stdexec::task<std::string> download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  for (std::int32_t i = 1; i <= 5; ++i) {
    if (progress) {
      progress->add(i * 20); // 20, 40, 60, 80, 100
      // 真实场景中事件来自异步工作；这里用可取消 sleep 模拟一个异步间隔。
      co_await dcb::sleep(std::chrono::milliseconds(10));
    }
  }
  co_return std::string("downloaded: ") + url;
}
```

### 生成的 Dart

```dart
Future<String> downloadWithProgress({
  required String url,
  StreamController<int>? progress,
}) => ...;
```

### Dart 用法

```dart
// 带进度事件：
final controller = StreamController<int>();
controller.stream.listen(progressValues.add);
final result = await downloadWithProgress(
  url: 'https://example.com/file.zip',
  progress: controller,
);
await controller.close(); // 用完后自己 close controller

// 不带进度：
final result = await downloadWithProgress(url: 'test.txt');
```

同一个请求同时携带流事件和最终结果：流的 `streamData` / `streamEnd` 帧，
加上返回值对应的 `responseOk` 帧。

### 同步变体

`BRIDGE_SYNC` 函数使用同样的 `std::optional<dcb::StreamSink<T>>` 参数；生成的
Dart API 是同步的：

```cpp
BRIDGE_SYNC
std::string sync_download_with_progress(
    std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress);
```

### C++ 实现

同步函数**不要**在阻塞 FFI 调用内部直接发事件——启动一个协程异步发送，然后立刻返回结果：

```cpp
namespace {
// 自由协程函数：参数会拷贝进协程帧，避免协程 lambda 的 capture 悬空问题。
stdexec::task<void> emit_progress(dcb::StreamSink<std::int32_t> sink) {
  for (std::int32_t i = 1; i <= 5; ++i) {
    sink.add(i * 20);  // 20, 40, 60, 80, 100
  }
  co_return;
}
}  // namespace

std::string sync_download_with_progress(
  std::string url, std::optional<dcb::StreamSink<std::int32_t>> progress) {
  if (progress) {
    // 应用层辅助函数：封装 stdexec::spawn 和由应用持有的 scope。
    start_progress_task(std::move(*progress));
  }
  return std::string("downloaded: ") + url;
}
```

事件会在 FFI 调用期间排队到 reply port，调用返回后立即送达 controller。请通过
scope-owned 的 stdexec spawn 启动 task，并在 Runtime 关闭时排空 scope；不要用协程 lambda，
因为临时 lambda 对象销毁后其 capture 会悬空。

```dart
final controller = StreamController<int>();
controller.stream.listen(progressValues.add);
final result = syncDownloadWithProgress(
  url: 'https://example.com/file.zip',
  progress: controller,
);
// result 立即可用；进度事件随后到达。
await Future<void>.delayed(const Duration(milliseconds: 20));
expect(progressValues, [20, 40, 60, 80, 100]);
await controller.close();
```

## 错误

- `sink.error(message)` 会向 Dart stream 发送错误事件并关闭流——这是流级错误的标准表达方式。
- 必需 stream 的业务函数通常立刻返回，不要依赖 dispatch 路径抛异常，请用 `sink.error()`。
- 可选 stream 的协程抛异常会让返回的 `Future` 失败，但 bridge 不会自动关闭 `StreamController`；
  需要在 C++ 里调 `progress->error(...)`，或在 Dart 里 `controller.close()`。

## 取消订阅

- Dart `sub.cancel()` 会发送 `dcb_stream_close`，原生侧把流标记为关闭。
- C++ 侧**不会被打断**：业务继续跑，之后的 `add()` / `end()` 静默丢弃。
- 再次调用会创建全新 stream（新的 stream id），取消后重新订阅没有问题
  （codegen_demo 的 stress 测试覆盖了该场景）。

## 线程与生命周期

- `add()` / `end()` / `error()` 线程安全，可从 io 线程、线程池线程或外部事件循环线程调用
  （见 codegen_demo 的 `uv_stream` / `worker_stream`：用 mpsc 通道把外部线程的数据转发到 io 上的 sink）。
- sink 内部持有 `std::shared_ptr<Session>`，可以活得比 dispatch 帧甚至 session 关闭更久；
  session dispose 后，晚到的调用靠 generation 检查静默丢弃，不会触碰已释放内存。

## Opaque 类成员 stream

Opaque 类的成员函数同样可以暴露 stream：

```cpp
class BRIDGE_OPAQUE Counter {
 public:
  void tickStream(dcb::StreamSink<std::int32_t> sink, std::int32_t count = 5,
                  std::int32_t intervalMs = 10);
};
```

```dart
final counter = Counter.int32T(initialValue: 3);
final values = await counter.tickStream(count: 3, intervalMs: 10).toList();
// values == [3, 3, 3]
```

## 延伸阅读

- [函数标记选择指南](/dart_cpp_bridge/zh-cn/guides/fundamentals/markers/)
- [基础运行时](/dart_cpp_bridge/zh-cn/guides/fundamentals/runtime/)
- [异常与错误处理](/dart_cpp_bridge/zh-cn/guides/fundamentals/errors/)
- [类型映射](/dart_cpp_bridge/zh-cn/codegen/type-mapping/)
