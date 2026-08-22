---
title: Type Mapping
description: C++ ↔ Dart type mapping rules, including data classes and opaque classes
sidebar:
  order: 3
---

:::note[v2.1.0]
The examples on this page use `stdexec::task` and `dcb::sync_wait`. The Dart
type mappings and wire encodings are shared with v1, but v1 C++ async signatures
use async-simple. See [Versioned Documentation](/dart_cpp_bridge/versions/).
:::

## Basic Types

| C++ Type | Dart Type | Description |
|----------|-----------|-------------|
| `bool` | `bool` | 1-byte encoding |
| `int` / `std::int32_t` | `int` | Signed 32-bit integer |
| `std::uint8_t` / `std::uint32_t` | `int` | Unsigned integer |
| `std::int64_t` | `int` | Signed 64-bit integer |
| `float` | `double` | 32-bit floating point |
| `double` | `double` | 64-bit floating point |
| `std::string` | `String` | Transmitted as raw bytes, not truncated by `\0` |
| `std::chrono::system_clock::time_point` | `DateTime` | `i64` Unix microseconds timestamp, no timezone info |
| `dcb::Int128` / `dcb::UInt128` | `BigInt` | 128-bit value encoded as a decimal string |

The public codegen primitive whitelist is the table above. Although
`int8_t`, `int16_t`, `uint16_t`, and `uint64_t` may appear in lower-level codec
or FFI fields, they are not public codegen API types. `u64` is used internally
for frame fields, object handles, and pointer addresses.

### Raw byte pointers

The generator has one deliberate pointer exception:

| C++ Type | Dart Type | Wire representation |
|----------|-----------|---------------------|
| `std::uint8_t*` | `Pointer<Uint8>` | Native address as `u64` |
| `const std::uint8_t*` | `Pointer<Uint8>` | Native address as `u64` |

This is address-passing mode, not a general zero-copy serializer. The pointer
parameter carries only an address; it carries no length and the bytes are not
copied by the wire codec. Add a separate length parameter and keep the memory
alive until the native operation has finished. `const` expresses C++ input
intent; the generated Dart type remains `Pointer<Uint8>`.

The generator recognizes this mapping in exported APIs. Other raw pointers
(`int32_t*`, `char*`, `void*`, and so on), references, and smart pointers
remain unsupported. Because the wire carries only an address, use pointers
only when the API explicitly documents the buffer lifetime and length.

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
| `std::vector<uint8_t>` | `Uint8List` |
| `std::vector<T>` (other) | `List<T>` |
| `std::array<T, N>` | `List<T>`, with a Dart-side length check for `N` |
| `std::vector<bool>` | `List<bool>` |

Only `std::vector<uint8_t>` has a dedicated typed-list mapping. Other vectors
and all arrays are generated as ordinary Dart `List` values; the array length
is checked by the generated Dart code.

### `std::optional` → Nullable Type

| C++ Type | Dart Type |
|----------|-----------|
| `std::optional<T>` | `T?` |

The presence tag and byte layout are documented in
[Wire Encoding and Runtime Codec](/dart_cpp_bridge/guides/fundamentals/encoding/);
this page only documents the generated nullable Dart type.

### `map / set`

| C++ Type | Dart Type |
|----------|-----------|
| `std::unordered_map<K, V>` | `Map<K, V>` |
| `std::unordered_set<T>` | `Set<T>` |
| `std::map<K, V>` | `Map<K, V>` |
| `std::set<T>` | `Set<T>` |

Both ordered and unordered containers are supported. They have the same Dart
types and payload shape; do not rely on iteration order for unordered
containers.

### `std::pair` / `std::tuple` → Dart Record

| C++ Type | Dart Type |
|----------|-----------|
| `std::pair<T1, T2>` | `(T1, T2)` |
| `std::tuple<T1, T2, ...>` | `(T1, T2, ...)` |

Mapped position-by-position; see
[Wire Encoding and Runtime Codec](/dart_cpp_bridge/guides/fundamentals/encoding/)
for the byte layout.

## DartFn Reverse Callbacks

| C++ Type | Dart Type |
|----------|-----------|
| `dcb::DartFn<Ret(Args...)>` | `Future<Ret> Function(Args...)` |

Supports any number of arguments; the Dart side generates the corresponding multi-argument closure.

### Async Calls (functor `operator()`)

```cpp
// Call Dart closures inside a coroutine via co_await, without blocking the io thread
BRIDGE_ASYNC
stdexec::task<std::string> greet_dart_fn(
    dcb::DartFn<std::string(std::string)> callback, std::string name);
```

```cpp
// Implementation: DartFn is a functor; operator() returns a sender
auto reply = co_await callback(name);
co_return "hello, " + reply;
```

### Blocking Calls (`dcb::sync_wait`)

```cpp
// Blocks the current thread until Dart replies — must be used in a thread pool (BRIDGE_NORMAL)
BRIDGE_NORMAL
std::string concat_dart_fn(
    dcb::DartFn<std::string(std::string, std::string)> callback,
    std::string a, std::string b);
```

```cpp
// Implementation: block-wait via dcb::sync_wait
auto reply = dcb::sync_wait(callback(a, b));
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
- `dcb::sync_wait(fn(args...))`: blocks the calling thread until Dart replies, **must be used in `BRIDGE_NORMAL` (thread pool)**, forbidden on the io thread
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

// Trigger mode A: BRIDGE_NORMAL (thread pool), call the stored closure via dcb::sync_wait
BRIDGE_NORMAL
std::string invoke_registered(std::string input);

// Trigger mode B: BRIDGE_ASYNC (coroutine), call via co_await fn(...), does not block the io thread
BRIDGE_ASYNC
stdexec::task<std::string> invoke_registered_async(std::string input);
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
  auto reply = dcb::sync_wait(g_registered_fn(input));  // safe: runs off the io thread
  return "registered:" + reply;
}

stdexec::task<std::string> invoke_registered_async(std::string input) {
  if (!g_registered_fn) throw std::runtime_error("no registered dart fn");
  auto reply = co_await g_registered_fn(input);  // coroutine suspends, io is not blocked
  co_return "async_registered:" + reply;
}
```

```dart
// Dart side usage
final ok = registerDartFn(callback: (s) => 'echo:$s');  // synchronous, only stores

// Mode A: thread pool dcb::sync_wait
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
| `invokeRegistered()` | Thread pool (`BRIDGE_NORMAL`) | Yes, `dcb::sync_wait` blocks a pool thread |
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
#include <stdexec/execution.hpp>

namespace demo::api {

// BRIDGE_SYNC — runs synchronously on the io thread, returns result immediately
// Good for: pure computation, non-blocking, microsecond-scale operations
BRIDGE_SYNC
std::int32_t bridge_version();

// BRIDGE_ASYNC — C++20 coroutine, scheduled on the io thread, can suspend via co_await
// Good for: async IO, coroutine composition, waiting for other async operations
BRIDGE_ASYNC
stdexec::task<std::int32_t> add(std::int32_t a, std::int32_t b);

// BRIDGE_NORMAL — ordinary function, posted to the thread pool for execution
// Good for: CPU-intensive computation, blocking file/network IO, any blocking operation
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

}  // namespace demo::api
```

### Rules

- `BRIDGE_SYNC`: the return value is directly encoded and returned; the Dart side is a synchronous call (returns `T`)
- `BRIDGE_ASYNC`: the return type must be `stdexec::task<T>` or a supported sender; the Dart side is `Future<T>`
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

`bool`, `int` / `std::int32_t`, `std::uint8_t`, `std::uint32_t`,
`std::int64_t`, `float`, `double`, `std::string`,
`std::chrono::system_clock::time_point`, `dcb::Int128` / `dcb::UInt128`

Example: `std::string name;`

#### Enums

`enum class T : std::int32_t` (must be marked `BRIDGE_EXPORT`)

Example: `enum class Color : std::int32_t { kRed = 0, kGreen = 1, kBlue = 2 };`

#### Containers

`std::vector<T>`, `std::array<T, N>`, `std::optional<T>`,
`std::map<K, V>`, `std::unordered_map<K, V>`, `std::set<T>`,
`std::unordered_set<T>`, `std::pair<T1, T2>`, `std::tuple<T1, ...>`

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
- `dcb::Int128` / `dcb::UInt128` map to Dart `BigInt` and use decimal-string encoding.
- Fields do not support pointers, references, opaque classes, raw C arrays, bitfields, unions, `std::variant`, `std::any`, etc.
- Container elements must also be whitelist types (e.g., `std::vector<AnotherDataClass>` is valid, `std::vector<std::unique_ptr<T>>` is invalid).
- Nested nullables like `std::optional<std::optional<T>>` are not supported.

### Wire Encoding

Fields are transferred in C++ **declaration order**, without field names. The
frame structure and byte layout are documented in
[Wire Encoding and Runtime Codec](/dart_cpp_bridge/guides/fundamentals/encoding/).

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
    stdexec::task<std::int32_t> value() const;

    // Thread pool method
    BRIDGE_NORMAL
    std::int64_t heavy_compute(std::int32_t rounds);

    // Static method
    BRIDGE_SYNC
    static std::int32_t sum(std::int32_t a, std::int32_t b);

    // Method returning its own type — this is not a factory, just an ordinary async method
    BRIDGE_ASYNC
    stdexec::task<Counter> duplicate() const;

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

## Large buffers: address-passing mode

The normal `std::vector<uint8_t>` mapping is `Uint8List`: the bytes are
serialized into the bridge frame. For a large native buffer, use the
`uint8_t*` exception above and pass the length explicitly. The bridge still
serializes the address and length fields, but it does not copy the pointed-to
bytes.

The codegen demo contains a complete sync and async round trip:
[`bridge_api.h`](https://github.com/deretame/dart_cpp_bridge/blob/main/examples/codegen_demo/native/api/bridge_api.h)
and
[`api_test.dart`](https://github.com/deretame/dart_cpp_bridge/blob/main/examples/codegen_demo/integration_test/api_test.dart).

```cpp
// Header: the length is part of the API because Pointer<Uint8> has no length.
BRIDGE_SYNC
std::uint8_t* echo_bytes(const std::uint8_t* data, std::int32_t len);

BRIDGE_ASYNC
stdexec::task<std::uint8_t*> async_echo_bytes(
    const std::uint8_t* data, std::int32_t len);
```

```cpp
// Implementation: the returned address must remain valid after the call.
namespace {
thread_local std::vector<std::uint8_t> echo_buffer(256);
}

std::uint8_t* echo_bytes(const std::uint8_t* data, std::int32_t len) {
  if (len < 0 || len > static_cast<std::int32_t>(echo_buffer.size())) {
    throw std::runtime_error("length out of range");
  }
  if (len != 0) {
    std::memcpy(echo_buffer.data(), data, static_cast<std::size_t>(len));
  }
  return echo_buffer.data();
}

stdexec::task<std::uint8_t*> async_echo_bytes(
    const std::uint8_t* data, std::int32_t len) {
  co_return echo_bytes(data, len);
}
```

```dart
import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

final input = Uint8List.fromList([10, 20, 30, 40]);
final nativeInput = calloc<Uint8>(input.length);
try {
  nativeInput.asTypedList(input.length).setAll(0, input);
  final nativeOutput = echoBytes(data: nativeInput, len: input.length);
  final output = nativeOutput.asTypedList(input.length);
  print(output); // [10, 20, 30, 40]
} finally {
  calloc.free(nativeInput);
}
```

Ownership is part of the function contract, not part of the generated type:

- Dart-owned input memory must stay allocated until a sync call returns or an
  async `Future` completes.
- C++ must validate the address and length before reading or writing. The
  bridge cannot validate an arbitrary native pointer.
- A returned pointer is only valid for the lifetime documented by C++. The demo
  returns a thread-local buffer; it is not owned by Dart and must not be passed
  to `calloc.free`.
- An address is process-local and must not be persisted, sent to another
  process, or assumed to be valid in another isolate.

For ordinary data, prefer `std::vector<uint8_t>` → `Uint8List`; it is safer and
has value semantics.

:::caution[Caution]
Never free or reuse a buffer while native code may still access it. A pointer
is not a capability or a lifetime handle, and the code generator does not add
ownership or bounds checks.
:::

---

## Unsupported Types

The following types are currently **unsupported**; codegen will raise an error
when encountering them:

- Raw pointers other than `std::uint8_t*` / `const std::uint8_t*` (for example
  `int32_t*`, `char*`, and `void*`)
- `std::unique_ptr<T>` and `std::shared_ptr<T>`
- References (`T&`) as parameters/return values
- `std::variant`, `std::any`
- Bitfields, unions
- Nested nullables (`std::optional<std::optional<T>>`)
- Unspecialized templates, static member variables
