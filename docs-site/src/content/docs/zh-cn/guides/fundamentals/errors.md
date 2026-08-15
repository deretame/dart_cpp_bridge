---
title: 异常与错误处理
description: C++ 异常如何透传到 Dart，以及 Dart 闭包异常如何回到 C++
---

:::caution[v2 开发线]
异步示例使用 `stdexec::task`。wire 错误帧和 Dart `StateError` 行为与 v1 共用。
:::

bridge 的 wire 层会把所有 C++ 异常捕获并编码为错误帧，再交给 Dart；Dart 闭包抛出的异常也会通过 wire 回到 C++。这页说明每种路径的传递规则。

## 核心原则

**异常永远不会跨越 FFI 边界。** 无论 C++ 还是 Dart 抛出的异常，都会被 bridge 捕获后编码成 wire 错误帧。

C++ 侧的统一处理：

```cpp
try {
  // 调用用户函数
} catch (const std::exception& e) {
  post_error(req, e.what());
} catch (...) {
  post_error(req, "unknown error");
}
```

## C++ 异常 → Dart

### BRIDGE_SYNC

```cpp
BRIDGE_SYNC
std::int32_t divide(std::int32_t a, std::int32_t b) {
  if (b == 0) throw std::runtime_error("divide by zero");
  return a / b;
}
```

Dart 侧：

```dart
try {
  final r = bridge.divide(a: 10, b: 0);
} on StateError catch (e) {
  print(e.message); // "divide by zero"
}
```

### BRIDGE_ASYNC

协程中抛出的异常（包括 `co_await` 抛出的）会被 wire dispatch 捕获。

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> fail() {
  throw std::runtime_error("async failed");
  co_return "";
}
```

Dart 侧：

```dart
try {
  await bridge.fail();
} on StateError catch (e) {
  print(e.message); // "async failed"
}
```

### BRIDGE_NORMAL

blocking pool 中的异常会通过 sender completion channel 传播，并在到达 Dart 前
由 wire dispatch 捕获。

```cpp
BRIDGE_NORMAL
std::string normal_fail() {
  throw std::runtime_error("normal failed");
}
```

Dart 侧同样收到 `StateError`。

### spawn_blocking

```cpp
stdexec::task<int> compute() {
  auto result = co_await dcb::spawn_blocking([] {
    throw std::runtime_error("blocking failed");
    return 0;
  });
  co_return result;
}
```

异常会在线程池被捕获，然后在 `co_await` 处重新抛出。如果你在 `BRIDGE_ASYNC` 里调用，最终会被 wire 捕获传给 Dart。

## Dart 异常 → C++

当 C++ 调用 Dart 闭包（`DartFn`），Dart 闭包里抛出的异常会传回 C++。

### Dart 侧

```dart
Future<String> greet(String name) async {
  if (name.isEmpty) throw Exception('name cannot be empty');
  return 'Hello, $name!';
}
```

### C++ 侧

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> call_greet(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  try {
    auto reply = co_await callback(name);
    co_return reply;
  } catch (const std::runtime_error& e) {
    // e.what() 包含 "Exception: name cannot be empty"
    co_return std::string("error: ") + e.what();
  }
}
```

## 错误帧格式

`responseErr` 帧的 payload：

```text
code      i32   错误码（生成的 dispatch 当前使用 1）
message   string 错误信息
```

Dart 侧生成代码会把它转成 `StateError`。

## 建议

- 业务级错误优先用**返回值**或 `std::optional` 表达，不要依赖异常
- 异常留给真正的不可恢复问题
- 不要捕获 `(...)` 后吞掉异常，至少要记录日志

## 延伸阅读

- [Wire 协议](/dart_cpp_bridge/reference/wire-protocol/)
- [函数标记选择指南](/dart_cpp_bridge/guides/fundamentals/markers/)
