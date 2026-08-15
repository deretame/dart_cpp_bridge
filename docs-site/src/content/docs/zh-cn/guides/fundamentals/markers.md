---
title: 标记选择（v2）
description: v2 中如何选择 BRIDGE_SYNC、BRIDGE_ASYNC、BRIDGE_NORMAL、Stream 和 DartFn
---

bridge 使用 `BRIDGE_*` 标记选择 dispatch 行为。标记名称和 Dart 侧返回
类型在 v1/v2 中保持稳定，变化的是 C++ 异步返回类型。

| 标记 | 执行上下文 | v2 C++ 签名 | Dart 返回 | 是否可阻塞 |
| --- | --- | --- | --- | --- |
| `BRIDGE_SYNC` | bridge io 线程 | `T` | `T` | 否 |
| `BRIDGE_ASYNC` | io scheduler，可挂起 task | `stdexec::task<T>` | `Future<T>` | 否 |
| `BRIDGE_NORMAL` | blocking thread pool | `T` | `Future<T>` | 是 |
| Stream | 由标记 + `StreamSink<T>` 决定 | 通常为 `void` 或 `stdexec::task<void>` | `Stream<T>` | 视实现而定 |

## `BRIDGE_SYNC`

适合短时间、非阻塞工作：

```cpp
BRIDGE_SYNC
std::int32_t bridge_version() { return 42; }
```

不要在 sync 函数中执行文件/网络 I/O、sleep、可能阻塞的锁，也不要调用
DartFn。io 线程必须快速返回。

## `BRIDGE_ASYNC`

适合异步 I/O、定时器、通道和会挂起的 DartFn 调用：

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> fetch_name(std::string id) {
  auto value = co_await dcb::fetch_from_channel(id);
  co_return value;
}
```

task 挂起时不会占用 io 线程。真正阻塞的工作放到
`dcb::spawn_blocking`，或直接使用 `BRIDGE_NORMAL`。

## `BRIDGE_NORMAL`

适合可能阻塞的普通函数：

```cpp
BRIDGE_NORMAL
std::string read_file(std::string path) {
  return read_file_synchronously(path);
}
```

生成的 dispatch 会在线程池执行函数，Dart 侧得到 `Future<String>`。

## Stream

当函数同时具有导出标记和必需的 `dcb::StreamSink<T>` 参数时，会生成 Dart
Stream。sync、async、normal 函数还可以使用可选的
`std::optional<dcb::StreamSink<T>>`。

```cpp
BRIDGE_NORMAL
void ticks(dcb::StreamSink<std::int32_t> sink, std::int32_t count) {
  for (std::int32_t i = 0; i < count; ++i) {
    sink.add(i);
  }
  sink.end();
}
```

取消订阅只会停止 Dart 侧接收；C++ 操作可能继续运行，之后的 sink 调用会
被静默丢弃。

## DartFn

v2 中 DartFn 是异步的，因为它返回 sender：

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> greet_dart(
    dcb::DartFn<std::string(std::string)> callback) {
  co_return co_await callback("from C++");
}
```

不要从 `BRIDGE_SYNC` 调用可能挂起的 DartFn。阻塞调用方需要结果时，应在
io 线程之外使用 `dcb::sync_wait`。

## 选择步骤

1. 能否快速完成且完全不阻塞？使用 `BRIDGE_SYNC`；
2. 是否需要等待 timer、channel、DartFn 或异步 I/O？使用 `BRIDGE_ASYNC`，
   返回 `stdexec::task<T>`；
3. 是否执行阻塞工作？使用 `BRIDGE_NORMAL`，或用
   `dcb::spawn_blocking` 卸载阻塞部分；
4. 是否要持续推送数据？增加必需的 `StreamSink<T>` 和导出标记。

另见 [v2 stdexec 异步 C++](/dart_cpp_bridge/zh-cn/guides/fundamentals/stdexec/)
和[异常与错误处理](/dart_cpp_bridge/zh-cn/guides/fundamentals/errors/)。
