---
title: Type Mapping
description: C++ ↔ Dart type mapping rules, including data classes and opaque classes
sidebar:
  order: 3
---

## Basic Types

| C++ Type | Dart Type | Description |
|----------|-----------|-------------|
| `bool` | `bool` | 1-byte encoding |
| `int8_t` / `int16_t` / `int32_t` / `int64_t` | `int` | Signed integers |
| `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` | `int` | Unsigned integers |
| `float` | `double` | 32-bit floating point |
| `double` | `double` | 64-bit floating point |
| `std::string` | `String` | Transmitted as raw bytes, not truncated by `\0` |
| `std::chrono::system_clock::time_point` | `DateTime` | `i64` Unix microseconds timestamp, no timezone info |
| `Int128` / `UInt128` | `BigInt` | Marker-only, not usable in practice |

## Enums

| C++ Type | Dart Type |
|----------|-----------|
| `enum class BRIDGE_EXPORT T : std::int32_t` | `enum T` |

```cpp
enum class BRIDGE_EXPORT OrderStatus : std::int32_t {
  kCreated = 0,
  kPaid = 1,
  kShipped = 2,
};
```

Generated:

```dart
enum OrderStatus { created, paid, shipped }
```

Rules:
- Must be marked `BRIDGE_EXPORT` to be exported; unmarked enums are not included in the IR
- Underlying type **only supports `std::int32_t`**; other types (e.g., `uint8_t`, `int64_t`) will raise an error
- Each enum constant **must explicitly specify a value** (e.g., `kCreated = 0`); omission is not allowed
- Transmitted over the wire as `int32_t`
- Complex enums are not supported; only plain enums are supported

## Containers

### `std::vector` / `std::array` → `List`

| C++ Type | Dart Type |
|----------|-----------|
| `std::vector<uint8_t>` / `std::array<uint8_t, N>` | `Uint8List` |
| `std::vector<int8_t>` / `std::array<int8_t, N>` | `Int8List` |
| `std::vector<int16_t>` / `std::array<int16_t, N>` | `Int16List` |
| `std::vector<int32_t>` / `std::array<int32_t, N>` | `Int32List` |
| `std::vector<int64_t>` / `std::array<int64_t, N>` | `Int64List` |
| `std::vector<uint16_t>` / `std::array<uint16_t, N>` | `Uint16List` |
| `std::vector<uint32_t>` / `std::array<uint32_t, N>` | `Uint32List` |
| `std::vector<uint64_t>` / `std::array<uint64_t, N>` | `Uint64List` |
| `std::vector<float>` / `std::array<float, N>` | `Float32List` |
| `std::vector<double>` / `std::array<double, N>` | `Float64List` |
| `std::vector<T>` / `std::array<T, N>` (other) | `List<T>` |
| `std::vector<bool>` / `std::array<bool, N>` | `List<bool>` |

Fixed-width integer/float elements **prefer typed lists** (`Uint8List`, `Int32List`, etc.) to avoid boxing. There is no `BoolList`; `std::vector<bool>` and `std::array<bool, N>` fall back to `List<bool>`.

### `std::optional` → Nullable Type

| C++ Type | Dart Type |
|----------|-----------|
| `std::optional<T>` | `T?` |

Wire encoding uses a presence tag: `Some(T)` = tag 1 + encoded `T`, `None` = tag 0.

### `std::unordered_map` / `std::unordered_set`

| C++ Type | Dart Type |
|----------|-----------|
| `std::unordered_map<K, V>` | `Map<K, V>` |
| `std::unordered_set<T>` | `Set<T>` |

### `std::pair` / `std::tuple` → Dart Record

| C++ Type | Dart Type |
|----------|-----------|
| `std::pair<T1, T2>` | `(T1, T2)` |
| `std::tuple<T1, T2, ...>` | `(T1, T2, ...)` |

Mapped position-by-position; elements are encoded on the wire in order, without length or field names.

## DartFn Reverse Callbacks

| C++ Type | Dart Type |
|----------|-----------|
| `dcb::DartFn<Ret(Args...)>` | `Future<Ret> Function(Args...)` |

Supports any number of arguments; the Dart side generates the corresponding multi-argument closure.

### Async Calls (functor `operator()`)

```cpp
// Call Dart closures inside a coroutine via co_await, without blocking the io thread
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> greet_dart_fn(
    dcb::DartFn<std::string(std::string)> callback, std::string name);
```

```cpp
// Implementation: DartFn is a functor; operator() returns Lazy<Ret>
auto reply = co_await callback(name);
co_return "hello, " + reply;
```

### Blocking Calls (`syncAwait`)

```cpp
// Blocks the current thread until Dart replies — must be used in a thread pool (BRIDGE_NORMAL)
BRIDGE_NORMAL
std::string concat_dart_fn(
    dcb::DartFn<std::string(std::string, std::string)> callback,
    std::string a, std::string b);
```

```cpp
// Implementation: block-wait via syncAwait + spawn
auto reply = async_simple::coro::syncAwait(dcb::spawn(callback(a, b)));
return "sync:" + reply;
```

### Dart Generated Shape

```dart
// Async version
Future<String> greetDartFn({
  required Future<String> Function(String) callback,
  required String name,
}) => BridgeApiImpl.instance.greetDartFn(callback, name);

// Blocking version (two arguments)
Future<String> concatDartFn({
  required Future<String> Function(String, String) callback,
  required String a,
  required String b,
}) => BridgeApiImpl.instance.concatDartFn(callback, a, b);
```

### Rules

- `co_await fn(args...)`: used inside a coroutine (`BRIDGE_ASYNC`), suspended via an oneshot channel, does not block the io thread
- `syncAwait(dcb::spawn(fn(args...)))`: blocks the calling thread until Dart replies, **must be used in `BRIDGE_NORMAL` (thread pool)**, forbidden on the io thread
- Dart closures must return a `Future` (async); the C++ side waits for the final result
- Supports multiple arguments: `DartFn<Ret(A1, A2, ...)>` corresponds to Dart `Future<Ret> Function(A1, A2, ...)`
- **`BRIDGE_SYNC` + DartFn callbacks are forbidden**: `dispatch_sync` runs on the Dart isolate thread, blocking that thread waiting for a Dart reply, causing a permanent deadlock

### Persistent Callbacks (`BRIDGE_PERSIST`)

By default, DartFn is **one-shot**: the Dart side automatically unregisters the callback after the function call ends. If you need the FRB-style "sync register + async invoke" pattern (store the closure and invoke it repeatedly), use the `BRIDGE_PERSIST` marker:

```cpp
// Register: BRIDGE_SYNC (isolate thread), only stores the closure, returns in microseconds
// BRIDGE_PERSIST: tells codegen not to auto-unregister DartFn
BRIDGE_SYNC
BRIDGE_PERSIST
bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback);

// Trigger mode A: BRIDGE_NORMAL (thread pool), call the stored closure via syncAwait
BRIDGE_NORMAL
std::string invoke_registered(std::string input);

// Trigger mode B: BRIDGE_ASYNC (coroutine), call via co_await fn(...), does not block the io thread
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> invoke_registered_async(std::string input);
```

```cpp
// Implementation
namespace {
dcb::DartFn<std::string(std::string)> g_registered_fn;
}

bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback) {
  g_registered_fn = std::move(callback);
  return static_cast<bool>(g_registered_fn);
}

std::string invoke_registered(std::string input) {
  if (!g_registered_fn) throw std::runtime_error("no registered dart fn");
  auto reply = async_simple::coro::syncAwait(dcb::spawn(g_registered_fn(input)));  // safe: runs in thread pool
  return "registered:" + reply;
}

async_simple::coro::Lazy<std::string> invoke_registered_async(std::string input) {
  if (!g_registered_fn) throw std::runtime_error("no registered dart fn");
  auto reply = co_await g_registered_fn(input);  // coroutine suspends, io is not blocked
  co_return "async_registered:" + reply;
}
```

```dart
// Dart side usage
final ok = registerDartFn(callback: (s) => 'echo:$s');  // synchronous, only stores

// Mode A: thread pool syncAwait
final r1 = await invokeRegistered(input: 'world');
print(r1); // registered:echo:world

// Mode B: coroutine co_await fn(...)
final r2 = await invokeRegisteredAsync(input: 'world');
print(r2); // async_registered:echo:world
```

**Why it doesn't deadlock:**

| Phase | Execution Location | Calls Dart Closure? |
|-------|--------------------|---------------------|
| `registerDartFn()` | Dart isolate thread (sync FFI) | No, only stores |
| `invokeRegistered()` | Thread pool (`BRIDGE_NORMAL`) | Yes, `syncAwait` blocks a pool thread |
| `invokeRegisteredAsync()` | io-thread coroutine (`BRIDGE_ASYNC`) | Yes, `co_await fn(...)` suspends the coroutine |

Neither mode deadlocks: the isolate event loop remains idle, so it can process port messages and reply normally.

:::caution[Caution]
`BRIDGE_PERSIST` callbacks are not automatically cleaned up. If the closure holds resources, the caller must manage the lifecycle themselves (e.g., register an empty callback or call `dispose`).
:::

---

## Free Functions

Ordinary functions within a namespace, divided into three markers by scheduling mode:

### Marking

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <async_simple/coro/Lazy.h>

namespace demo::api {

// BRIDGE_SYNC — runs synchronously on the io thread, returns result immediately
// Good for: pure computation, non-blocking, microsecond-scale operations
BRIDGE_SYNC
std::int32_t bridge_version();

// BRIDGE_ASYNC — C++20 coroutine, scheduled on the io thread, can suspend via co_await
// Good for: async IO, coroutine composition, waiting for other async operations
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);

// BRIDGE_NORMAL — ordinary function, posted to the thread pool for execution
// Good for: CPU-intensive computation, blocking file/network IO, any blocking operation
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

}  // namespace demo::api
```

### Rules

- `BRIDGE_SYNC`: the return value is directly encoded and returned; the Dart side is a synchronous call (returns `T`)
- `BRIDGE_ASYNC`: the return type must be `async_simple::coro::Lazy<T>`; the Dart side is `Future<T>`
- `BRIDGE_NORMAL`: an ordinary C++ function (no coroutine); the runtime automatically posts it to `asio::thread_pool`; the Dart side is `Future<T>`
- Function names are automatically converted from `snake_case` to Dart `camelCase`
- Parameters are generated as named parameters on the Dart side (`{required int a}`)
- C++ default parameters → Dart optional named parameters (`{int delta = 1}`)

### Dart Generated Shape

```dart
// BRIDGE_SYNC → synchronous return
int bridgeVersion() => BridgeApiImpl.instance.bridgeVersion();

// BRIDGE_ASYNC → Future
Future<int> add({required int a, required int b}) =>
    BridgeApiImpl.instance.add(a, b);

// BRIDGE_NORMAL → Future (thread pool)
Future<String> sleepGreeting({required String name}) =>
    BridgeApiImpl.instance.sleepGreeting(name: name);
```

### How to Choose

| Marker | Execution Location | Dart Return | Use Cases |
|--------|--------------------|-------------|-----------|
| `BRIDGE_SYNC` | io thread | `T` | Pure computation, extremely fast operations (< 1μs) |
| `BRIDGE_ASYNC` | io thread (coroutine) | `Future<T>` | Async IO, coroutine composition |
| `BRIDGE_NORMAL` | thread pool | `Future<T>` | Blocking operations, CPU-intensive |

> **Note**: `BRIDGE_SYNC` and `BRIDGE_ASYNC` both run on the io thread and must never block. If you need to call blocking APIs (file read/write, `sleep`, mutex waits, etc.), you must use `BRIDGE_NORMAL`.

---

## Data Classes

### Marking

```cpp
#include <dart_cpp_bridge/annotate.h>

struct BRIDGE_DATA_CLASS Point {
    double x;
    double y;
};

// Nested data class
struct BRIDGE_DATA_CLASS Rect {
    Point topLeft;
    Point bottomRight;
};
```

### Field Whitelist

Data class fields can only use the following types (all value semantics, encoded on the wire in order):

#### Basic Types

`bool`, `int8/16/32/64_t`, `uint8/16/32/64_t`, `float`, `double`, `std::string`, `std::chrono::system_clock::time_point`

Example: `std::string name;`

#### Enums

`enum class T : std::int32_t` (must be marked `BRIDGE_EXPORT`)

Example: `enum class Color : std::int32_t { kRed = 0, kGreen = 1, kBlue = 2 };`

#### Containers

`std::vector<T>`, `std::array<T, N>`, `std::optional<T>`, `std::unordered_map<K, V>`, `std::unordered_set<T>`, `std::pair<T1, T2>`, `std::tuple<T1, ...>`

Example: `std::vector<int32_t> ids;`

#### Nested Data Classes

Another `BRIDGE_DATA_CLASS` type.

Example:

```cpp
struct BRIDGE_DATA_CLASS Circle {
    Point center;
    double radius;
};
```

**Notes**:
- `Int128` / `UInt128` are marker-only and **cannot actually be used as fields**.
- Fields do not support pointers, references, opaque classes, raw C arrays, bitfields, unions, `std::variant`, `std::any`, etc.
- Container elements must also be whitelist types (e.g., `std::vector<AnotherDataClass>` is valid, `std::vector<std::unique_ptr<T>>` is invalid).
- Nested nullables like `std::optional<std::optional<T>>` are not supported.

### Wire Encoding

Fields are encoded on the wire in the **declaration order** from the C++ header, without field names:

```text
Point  → x (f64) + y (f64)
Rect   → topLeft.x + topLeft.y + bottomRight.x + bottomRight.y
```

### Dart Generated Shape

```dart
class Point {
  final double x;
  final double y;

  const Point({required this.x, required this.y});

  @override
  int get hashCode => x.hashCode ^ y.hashCode;

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is Point && runtimeType == other.runtimeType &&
          x == other.x && y == other.y;

  @override
  String toString() => 'Point(x: $x, y: $y)';
}
```

- Non-nullable fields use `required`; nullable fields (`std::optional<T>`) are optional named parameters
- Passed by value, can cross Isolate boundaries
- `toString()` is generated from fields by default; to customize, override with `dart_code` in `dart_cpp_bridge.yaml`, see [Configuration → Custom data class `toString()`](../configuration/#dart_code)

---

## Opaque Classes

Aligns with FRB's `RustAutoOpaque`: only annotated methods are generated, public fields are ignored.

### Marking

```cpp
#include <dart_cpp_bridge/annotate.h>

class BRIDGE_OPAQUE Counter {
 public:
    // Constructor — generates a Dart factory constructor
    BRIDGE_CONSTRUCTOR Counter(std::int32_t initialValue);
    BRIDGE_CONSTRUCTOR Counter();  // parameterless constructor → default factory

    // Synchronous instance method (with default parameters)
    BRIDGE_SYNC
    void increment(std::int32_t delta = 1);

    // Asynchronous instance method
    BRIDGE_ASYNC
    async_simple::coro::Lazy<std::int32_t> value() const;

    // Thread pool method
    BRIDGE_NORMAL
    std::int64_t heavy_compute(std::int32_t rounds);

    // Static method
    BRIDGE_SYNC
    static std::int32_t sum(std::int32_t a, std::int32_t b);

    // Method returning its own type — this is not a factory, just an ordinary async method
    BRIDGE_ASYNC
    async_simple::coro::Lazy<Counter> duplicate() const;

    // toString
    BRIDGE_TO_STRING
    std::string toString() const;

 private:
    std::int32_t count_ = 0;
};
```

### Rules

- Passed on the wire via **object handles**, entering a per-Session registry
- **Cannot be shared across Isolates**, cannot be passed by value
- Field access requires hand-written `BRIDGE_SYNC` getter/setter
- Not supported: inheritance/polymorphism, virtual functions, method overloading, copy/move constructors
- `BRIDGE_CONSTRUCTOR` can only mark real C++ constructors; each constructor generates one Dart factory constructor
- Methods returning their own type (e.g., `duplicate()`) are **not** factory functions, just ordinary async/sync methods

### Lifecycle

- Construction: registered into `ObjectHandleRegistry`, returns a handle
- Destruction: `NativeFinalizer` automatically calls `dcb_drop_object` on Dart GC
- Automatically releases all objects of that Session when the Session closes

### Dart Generated Shape

```dart
class Counter extends CppOpaqueInterface {
  Counter._({required super.bridge, required super.handle});

  // BRIDGE_CONSTRUCTOR Counter(int32_t) → named factory (named by parameter type)
  factory Counter.int32T({required int initialValue}) =>
      BridgeApiImpl.instance.counterNewWithInitialValue(initialValue: initialValue);

  // BRIDGE_CONSTRUCTOR Counter() → default factory
  factory Counter() =>
      BridgeApiImpl.instance.counterNew();

  Future<void> increment({int delta = 1}) =>
      BridgeApiImpl.instance.counterIncrement(this, delta);

  Future<int> value() => BridgeApiImpl.instance.counterValue(this);

  Future<int> heavyCompute(int rounds) =>
      BridgeApiImpl.instance.counterHeavyCompute(this, rounds);

  static int sum(int a, int b) => BridgeApiImpl.instance.counterSum(a, b);

  // Returns its own type → ordinary async method, not a factory
  Future<Counter> duplicate() => BridgeApiImpl.instance.counterDuplicate(this);

  @override
  String toString() => BridgeApiImpl.instance.counterToString(this);
}
```

- The first field of an instance method payload is the handle, not exposed on the Dart side
- C++ default parameters → Dart optional positional parameters
- `BRIDGE_TO_STRING` → Dart `toString()` override

---

## Large Buffers: Address-Passing Mode

dart_cpp_bridge's wire protocol is not zero-copy itself; frame data needs serialization/deserialization between Dart and C++. However, for everyday types (basic types, small data classes, short strings), this copy overhead is negligible and generally not a concern.

When you need to read/write large chunks of raw memory (images, audio samples, large arrays, etc.), it's recommended to allocate native memory on the Dart side, then pass only the two integers (address and length) to C++:

```cpp
BRIDGE_NORMAL
std::tuple<int64_t, int64_t, int64_t> process_buffer(
    int64_t address, int64_t length);
```

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';

final bufferSize = 1024 * 1024;
final buffer = calloc<Uint8>(bufferSize);

try {
  // Only two int64 values are passed, message copy overhead is negligible
  final (outAddr, outLen, checksum) = await processBuffer(
    address: buffer.address,
    length: bufferSize,
  );
  // If you need to read memory output by C++, continue accessing via outAddr/outLen
} finally {
  calloc.free(buffer);
}
```

Benefits of this approach:

- The real large buffer does not go through the message channel; Dart and C++ collaborate through the same native memory segment
- Only two `int64` values are transmitted on the wire, copy overhead is almost negligible
- Async code is written like ordinary functions; codegen automatically handles FFI calls and port callbacks

:::caution[Caution]
Memory allocated by Dart must be managed by yourself; remember to `calloc.free(buffer)` at the right time. The C++ side should not continue to access that address after Dart has freed the memory.
:::

---

## Unsupported Types

The following types are currently **unsupported**; codegen will raise an error when encountering them:

- Pointers (`T*`, `std::unique_ptr<T>`, `std::shared_ptr<T>`)
- References (`T&`) as parameters/return values
- `std::map`, `std::set` (ordered containers)
- `std::variant`, `std::any`
- Bitfields, unions
- Nested nullables (`std::optional<std::optional<T>>`)
- Unspecialized templates, static member variables
