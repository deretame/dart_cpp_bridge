---
title: Exceptions and Error Handling
description: How C++ exceptions propagate to Dart and how Dart closure exceptions return to C++
---

:::note[v2.1.0]
Async examples use `stdexec::task`. Wire error frames and Dart `StateError`
behavior are shared with v1.
:::

The bridge's wire layer catches all C++ exceptions and encodes them as error frames before handing them to Dart; exceptions thrown by Dart closures also return to C++ via the wire. This page explains the propagation rules for each path.

## Core Principles

**Exceptions never cross the FFI boundary.** Whether thrown from C++ or Dart, exceptions are caught by the bridge and encoded as wire error frames.

Unified handling on the C++ side:

```cpp
try {
  // call the user function
} catch (const std::exception& e) {
  post_error(req, e.what());
} catch (...) {
  post_error(req, "unknown error");
}
```

## C++ Exceptions → Dart

### BRIDGE_SYNC

```cpp
BRIDGE_SYNC
std::int32_t divide(std::int32_t a, std::int32_t b) {
  if (b == 0) throw std::runtime_error("divide by zero");
  return a / b;
}
```

Dart side:

```dart
try {
  final r = bridge.divide(a: 10, b: 0);
} on StateError catch (e) {
  print(e.message); // "divide by zero"
}
```

### BRIDGE_ASYNC

Exceptions thrown in coroutines (including those thrown by `co_await`) are caught by the wire dispatch.

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> fail() {
  throw std::runtime_error("async failed");
  co_return "";
}
```

Dart side:

```dart
try {
  await bridge.fail();
} on StateError catch (e) {
  print(e.message); // "async failed"
}
```

### BRIDGE_NORMAL

Exceptions from the blocking pool are propagated through the sender completion
channel and caught by the wire dispatch before reaching Dart.

```cpp
BRIDGE_NORMAL
std::string normal_fail() {
  throw std::runtime_error("normal failed");
}
```

Dart side also receives `StateError`.

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

The exception is caught in the thread pool and re-thrown at the `co_await`. If you call this inside `BRIDGE_ASYNC`, the wire layer eventually catches it and passes it to Dart.

## Dart Exceptions → C++

When C++ calls a Dart closure (`DartFn`), exceptions thrown by the Dart closure are returned to C++.

### Dart side

```dart
Future<String> greet(String name) async {
  if (name.isEmpty) throw Exception('name cannot be empty');
  return 'Hello, $name!';
}
```

### C++ side

```cpp
BRIDGE_ASYNC
stdexec::task<std::string> call_greet(
    dcb::DartFn<std::string(std::string)> callback, std::string name) {
  try {
    auto reply = co_await callback(name);
    co_return reply;
  } catch (const std::runtime_error& e) {
    // e.what() contains "Exception: name cannot be empty"
    co_return std::string("error: ") + e.what();
  }
}
```

## Error Frame Format

Payload of the `responseErr` frame:

```text
code      i32   error code (generated dispatch currently uses 1)
message   string error message
```

The Dart generated code converts this into a `StateError`.

## Recommendations

- Express business-level errors preferentially through **return values** or `std::optional`, rather than relying on exceptions
- Reserve exceptions for truly unrecoverable problems
- Do not swallow exceptions after catching `(...)`; at least log them

## Further Reading

- [Wire Protocol](/dart_cpp_bridge/reference/wire-protocol/)
- [Marker Selection Guide](/dart_cpp_bridge/guides/fundamentals/markers/)
