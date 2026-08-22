---
title: 类型映射
description: C++ ↔ Dart 类型映射规则，包括数据类与不透明类
sidebar:
  order: 3
---

:::note[v2.1.0]
本页示例使用 `stdexec::task` 和 `dcb::sync_wait`。Dart 类型映射和 wire 编码
与 v1 共用，但 v1 的 C++ 异步签名使用 async-simple。参见[版本文档](/dart_cpp_bridge/zh-cn/versions/)。
:::

## 基础类型

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `bool` | `bool` | 1 字节编码 |
| `int` / `std::int32_t` | `int` | 有符号 32 位整数 |
| `std::uint8_t` / `std::uint32_t` | `int` | 无符号整数 |
| `std::int64_t` | `int` | 有符号 64 位整数 |
| `float` | `double` | 32 位浮点 |
| `double` | `double` | 64 位浮点 |
| `std::string` | `String` | 按字节原样传输，不以 `\0` 截断 |
| `std::chrono::system_clock::time_point` | `DateTime` | i64 Unix 微秒时间戳，不带时区信息 |
| `dcb::Int128` / `dcb::UInt128` | `BigInt` | 十进制字符串编码的 128 位值类型 |

代码生成器的公开基础类型白名单是上表中的类型。`int8_t`、`int16_t`、
`uint16_t` 和 `uint64_t` 虽然可能出现在底层 codec 或 FFI 字段中，但目前
不是 codegen 的公开 API 类型；`u64` 主要用于 frame 字段、对象 handle 和
指针地址。
### 唯一支持的原始指针

代码生成器对原始指针只有一个特判：

| C++ 类型 | Dart 类型 | Wire 表示 |
|----------|-----------|-----------|
| `std::uint8_t*` | `Pointer<Uint8>` | native 地址，编码为 `u64` |
| `const std::uint8_t*` | `Pointer<Uint8>` | native 地址，编码为 `u64` |

这是一种“地址传递”模式，不是通用的零拷贝序列化。指针参数只携带地址，
不携带长度，wire codec 也不会复制指针指向的字节；长度必须作为独立参数
传入。`const` 只表达 C++ 的输入意图，生成的 Dart 类型仍然是
`Pointer<Uint8>`。

该映射用于导出函数或方法的参数/返回值。其他原始指针（如
`int32_t*`、`char*`、`void*`）、引用和智能指针仍然不支持；数据类
字段仍使用值类型白名单，也不应放入指针。

## 枚举

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `enum class BRIDGE_EXPORT T : std::int32_t` | `enum T` |

```cpp
enum class BRIDGE_EXPORT OrderStatus : std::int32_t {
  kCreated = 0,
  kPaid = 1,
  kShipped = 2,
};
```

生成：

```dart
enum OrderStatus { created, paid, shipped }
```

规则：
- 必须标记 `BRIDGE_EXPORT` 才会导出，未标记的枚举不会进入 IR
- 底层类型**仅支持 `std::int32_t`**，其他类型（如 `uint8_t`、`int64_t`）会报错
- 每个枚举常量**必须显式指定数值**（如 `kCreated = 0`），不允许省略
- 按 `int32_t` 在 wire 上传输
- 不支持复杂枚举，仅支持普通的枚举

## 容器

### std::vector / std::array → List

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `std::vector<uint8_t>` | `Uint8List` |
| `std::vector<T>`（其他） | `List<T>` |
| `std::array<T, N>` | `List<T>`，Dart 侧校验长度为 `N` |
| `std::vector<bool>` | `List<bool>` |

目前只有 `std::vector<uint8_t>` 有专门的 `Uint8List` 映射；其他容器生成
为普通 Dart `List`，避免把未实现的 typed-list 支持写成 API 保证。

### std::optional → 可空类型

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `std::optional<T>` | `T?` |

presence tag 和具体字节布局见 [Wire 编码与运行时编解码](/dart_cpp_bridge/zh-cn/guides/fundamentals/encoding/)；本页只说明它在 Dart 侧表现为可空类型。

### map / set

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `std::unordered_map<K, V>` | `Map<K, V>` |
| `std::unordered_set<T>` | `Set<T>` |
| `std::map<K, V>` | `Map<K, V>` |
| `std::set<T>` | `Set<T>` |

有序和无序容器都受 codegen 支持；Dart 侧都表现为 `Map` / `Set`。无序
容器的迭代顺序不应作为业务语义依赖。

### std::pair / std::tuple → Dart Record

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `std::pair<T1, T2>` | `(T1, T2)` |
| `std::tuple<T1, T2, ...>` | `(T1, T2, ...)` |

按位置一一对应；具体编码顺序见 [Wire 编码与运行时编解码](/dart_cpp_bridge/zh-cn/guides/fundamentals/encoding/)。

## DartFn 反向回调

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `dcb::DartFn<Ret(Args...)>` | `Future<Ret> Function(Args...)` |

支持任意数量参数，Dart 侧生成对应的多参数闭包。

### 异步调用（仿函数 operator()）

```cpp
// 在协程中通过 co_await 调用 Dart 闭包，不阻塞 io 线程
BRIDGE_ASYNC
stdexec::task<std::string> greet_dart_fn(
    dcb::DartFn<std::string(std::string)> callback, std::string name);
```

```cpp
// 实现：DartFn 是仿函数，operator() 返回 sender
auto reply = co_await callback(name);
co_return "hello, " + reply;
```

### 阻塞调用（`dcb::sync_wait`）

```cpp
// 阻塞当前线程直到 Dart 回复 — 必须在线程池中使用（BRIDGE_NORMAL）
BRIDGE_NORMAL
std::string concat_dart_fn(
    dcb::DartFn<std::string(std::string, std::string)> callback,
    std::string a, std::string b);
```

```cpp
// 实现：通过 dcb::sync_wait 阻塞等待
auto reply = dcb::sync_wait(callback(a, b));
return "sync:" + reply;
```

### Dart 生成形态

```dart
// 异步版本
Future<String> greetDartFn({
  required Future<String> Function(String) callback,
  required String name,
}) => BridgeApiImpl.instance.greetDartFn(callback, name);

// 阻塞版本（两个参数）
Future<String> concatDartFn({
  required Future<String> Function(String, String) callback,
  required String a,
  required String b,
}) => BridgeApiImpl.instance.concatDartFn(callback, a, b);
```

### 规则

- `co_await fn(args...)`：在协程内使用（`BRIDGE_ASYNC`），通过 oneshot channel 挂起，不阻塞 io 线程
- `dcb::sync_wait(fn(args...))`：阻塞调用线程直到 Dart 回复，**必须在 `BRIDGE_NORMAL`（线程池）中使用**，禁止在 io 线程调用
- Dart 闭包必须返回 `Future`（异步），C++ 侧等待最终结果
- 支持多参数：`DartFn<Ret(A1, A2, ...)>` 对应 Dart `Future<Ret> Function(A1, A2, ...)`
- **禁止** `BRIDGE_SYNC` + DartFn 回调：`dispatch_sync` 跑在 Dart isolate 线程上，阻塞该线程等待 Dart 回复，形成永久死锁

### 持久化回调（BRIDGE_PERSIST）

默认情况下，DartFn 是**一次性**的：函数调用结束后 Dart 侧自动注销回调。如果需要 FRB 风格的「同步注册 + 异步调用」模式（闭包存储后反复调用），使用 `BRIDGE_PERSIST` 标记：

```cpp
// 注册：BRIDGE_SYNC（isolate 线程），只存储闭包，微秒返回
// BRIDGE_PERSIST：告诉 codegen 不自动注销 DartFn
BRIDGE_SYNC
BRIDGE_PERSIST
bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback);

// 触发方式 A：BRIDGE_NORMAL（线程池），通过 dcb::sync_wait 调用已存储的闭包
BRIDGE_NORMAL
std::string invoke_registered(std::string input);

// 触发方式 B：BRIDGE_ASYNC（协程），通过 co_await fn(...) 调用，不阻塞 io 线程
BRIDGE_ASYNC
stdexec::task<std::string> invoke_registered_async(std::string input);
```

```cpp
// 实现
namespace {
dcb::DartFn<std::string(std::string)> g_registered_fn;
}

bool register_dart_fn(dcb::DartFn<std::string(std::string)> callback) {
  g_registered_fn = std::move(callback);
  return static_cast<bool>(g_registered_fn);
}

std::string invoke_registered(std::string input) {
  if (!g_registered_fn) throw std::runtime_error("no registered dart fn");
  auto reply = dcb::sync_wait(g_registered_fn(input));  // 安全：不在 io 线程等待
  return "registered:" + reply;
}

stdexec::task<std::string> invoke_registered_async(std::string input) {
  if (!g_registered_fn) throw std::runtime_error("no registered dart fn");
  auto reply = co_await g_registered_fn(input);  // 协程挂起，io 不阻塞
  co_return "async_registered:" + reply;
}
```

```dart
// Dart 侧使用
final ok = registerDartFn(callback: (s) => 'echo:$s');  // 同步，只存储

// 方式 A：线程池 dcb::sync_wait
final r1 = await invokeRegistered(input: 'world');
print(r1); // registered:echo:world

// 方式 B：协程 co_await fn(...)
final r2 = await invokeRegisteredAsync(input: 'world');
print(r2); // async_registered:echo:world
```

**为什么不死锁：**

| 阶段 | 执行位置 | 是否调用 Dart 闭包 |
|------|----------|-------------------|
| `registerDartFn()` | Dart isolate 线程（sync FFI） | 否，只存储 |
| `invokeRegistered()` | 线程池（BRIDGE_NORMAL） | 是，`dcb::sync_wait` 阻塞 pool 线程 |
| `invokeRegisteredAsync()` | io 线程协程（BRIDGE_ASYNC） | 是，`co_await fn(...)` 挂起协程 |

两种方式都不会死锁：isolate 事件循环始终空闲，可以正常处理 port 消息并回复。

:::caution[注意]
`BRIDGE_PERSIST` 的回调不会自动清理。如果闭包持有资源，调用者需要自行管理生命周期（例如注册一个空回调或调用 dispose）。
:::

---

## 自由函数

命名空间内的普通函数，按调度方式分三种标记：

### 标记方式

```cpp
#include <dart_cpp_bridge/annotate.h>
#include <stdexec/execution.hpp>

namespace demo::api {

// BRIDGE_SYNC — 在 io 线程同步执行，立即返回结果
// 适合：纯计算、无阻塞、微秒级操作
BRIDGE_SYNC
std::int32_t bridge_version();

// BRIDGE_ASYNC — C++20 协程，在 io 线程调度，可 co_await 挂起
// 适合：异步 IO、协程组合、需要等待其他异步操作
BRIDGE_ASYNC
stdexec::task<std::int32_t> add(std::int32_t a, std::int32_t b);

// BRIDGE_NORMAL — 普通函数，投递到线程池执行
// 适合：CPU 密集计算、阻塞式文件/网络 IO、任何会阻塞的操作
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

}  // namespace demo::api
```

### 规则

- `BRIDGE_SYNC`：返回值直接编码回传，Dart 侧为同步调用（返回 `T`）
- `BRIDGE_ASYNC`：返回类型必须是 `stdexec::task<T>` 或受支持 sender，Dart 侧为 `Future<T>`
- `BRIDGE_NORMAL`：普通 C++ 函数（无协程），运行时自动投递到 `asio::thread_pool`，Dart 侧为 `Future<T>`
- 函数名自动从 `snake_case` 转为 Dart `camelCase`
- 参数在 Dart 侧生成为命名参数（`{required int a}`）
- C++ 默认参数 → Dart 可选命名参数（`{int delta = 1}`）

### Dart 生成形态

```dart
// BRIDGE_SYNC → 同步返回
int bridgeVersion() => BridgeApiImpl.instance.bridgeVersion();

// BRIDGE_ASYNC → Future
Future<int> add({required int a, required int b}) =>
    BridgeApiImpl.instance.add(a, b);

// BRIDGE_NORMAL → Future（线程池）
Future<String> sleepGreeting({required String name}) =>
    BridgeApiImpl.instance.sleepGreeting(name: name);
```

### 如何选择

| 标记 | 执行位置 | Dart 返回 | 适用场景 |
|------|----------|-----------|----------|
| `BRIDGE_SYNC` | io 线程 | `T` | 纯计算、极快操作（< 1μs） |
| `BRIDGE_ASYNC` | io 线程（协程） | `Future<T>` | 异步 IO、协程组合 |
| `BRIDGE_NORMAL` | 线程池 | `Future<T>` | 阻塞操作、CPU 密集 |

> **注意**：`BRIDGE_SYNC` 和 `BRIDGE_ASYNC` 都在 io 线程执行，绝对不能阻塞。如果需要调用阻塞式 API（文件读写、`sleep`、互斥锁等待等），必须使用 `BRIDGE_NORMAL`。

---

## 数据类（Data Class）

### 标记方式

```cpp
#include <dart_cpp_bridge/annotate.h>

struct BRIDGE_DATA_CLASS Point {
    double x;
    double y;
};

// 嵌套数据类
struct BRIDGE_DATA_CLASS Rect {
    Point topLeft;
    Point bottomRight;
};
```

### 字段白名单

数据类字段只能使用以下类型（均为值语义，wire 按顺序编码）：

#### 基础类型

`bool`、`int` / `std::int32_t`、`std::uint8_t`、`std::uint32_t`、`std::int64_t`、`float`、`double`、`std::string`、`std::chrono::system_clock::time_point`、`dcb::Int128` / `dcb::UInt128`

示例：`std::string name;`

#### 枚举

`enum class T : std::int32_t`（必须标记 `BRIDGE_EXPORT`）

示例：`enum class Color : std::int32_t { kRed = 0, kGreen = 1, kBlue = 2 };`

#### 容器

`std::vector<T>`、`std::array<T, N>`、`std::optional<T>`、`std::map<K, V>`、
`std::unordered_map<K, V>`、`std::set<T>`、`std::unordered_set<T>`、
`std::pair<T1, T2>`、`std::tuple<T1, ...>`

示例：`std::vector<int32_t> ids;`

#### 嵌套数据类

另一个 `BRIDGE_DATA_CLASS` 类型。

示例：

```cpp
struct BRIDGE_DATA_CLASS Circle {
    Point center;
    double radius;
};
```

**注意**：
- `dcb::Int128` / `dcb::UInt128` 在生成的 Dart API 中对应 `BigInt`，按十进制字符串编码。
- 字段不支持指针、引用、不透明类、原始 C 数组、位域、联合体、`std::variant`、`std::any` 等。
- 容器元素也必须是白名单内类型（例如 `std::vector<AnotherDataClass>` 合法，`std::vector<std::unique_ptr<T>>` 不合法）。
- 不支持 `std::optional<std::optional<T>>` 等嵌套可空。

### Wire 编码

字段按 C++ 头文件中的**声明顺序**逐个传值，不传字段名。帧结构和字段的
具体字节布局见 [Wire 编码与运行时编解码](/dart_cpp_bridge/zh-cn/guides/fundamentals/encoding/)。

### Dart 生成形态

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

- 不可空字段用 `required`，可空字段（`std::optional<T>`）为可选命名参数
- 按值传递，可跨 Isolate
- `toString()` 默认按字段生成；如需自定义，可在 `dart_cpp_bridge.yaml` 中用 `dart_code` 覆盖，参见 [配置 → 自定义数据类 toString()](../configuration/#dart_code)

---

## 不透明类（Opaque）

对齐 FRB 的 `RustAutoOpaque`：只生成标注的方法，公开字段被忽略。

### 标记方式

```cpp
#include <dart_cpp_bridge/annotate.h>

class BRIDGE_OPAQUE Counter {
 public:
    // 构造函数 — 生成 Dart factory 构造器
    BRIDGE_CONSTRUCTOR Counter(std::int32_t initialValue);
    BRIDGE_CONSTRUCTOR Counter();  // 无参构造 → 默认 factory

    // 同步实例方法（带默认参数）
    BRIDGE_SYNC
    void increment(std::int32_t delta = 1);

    // 异步实例方法
    BRIDGE_ASYNC
    stdexec::task<std::int32_t> value() const;

    // 线程池方法
    BRIDGE_NORMAL
    std::int64_t heavy_compute(std::int32_t rounds);

    // 静态方法
    BRIDGE_SYNC
    static std::int32_t sum(std::int32_t a, std::int32_t b);

    // 返回自身类型的方法 — 这不是工厂，只是普通异步方法
    BRIDGE_ASYNC
    stdexec::task<Counter> duplicate() const;

    // toString
    BRIDGE_TO_STRING
    std::string toString() const;

 private:
    std::int32_t count_ = 0;
};
```

### 规则

- 通过**对象句柄**在 wire 上传递，进入 per-Session 注册表
- **不能跨 Isolate 共享**，不能按值传递
- 字段访问需手写 `BRIDGE_SYNC` getter/setter
- 不支持：继承多态、虚函数、方法重载、拷贝/移动构造
- `BRIDGE_CONSTRUCTOR` 只能标记真正的 C++ 构造函数，每个构造函数生成一个 Dart factory 构造器
- 返回自身类型的方法（如 `duplicate()`）**不是**工厂函数，只是普通的异步/同步方法

### 生命周期

- 构造：注册到 `ObjectHandleRegistry`，返回 handle
- 析构：Dart GC 时 `NativeFinalizer` 自动调用 `dcb_drop_object`
- Session 关闭时自动释放该 Session 全部对象

### Dart 生成形态

```dart
class Counter extends CppOpaqueInterface {
  Counter._({required super.bridge, required super.handle});

  // BRIDGE_CONSTRUCTOR Counter(int32_t) → 命名 factory（按参数类型命名）
  factory Counter.int32T({required int initialValue}) =>
      BridgeApiImpl.instance.counterNewWithInitialValue(initialValue: initialValue);

  // BRIDGE_CONSTRUCTOR Counter() → 默认 factory
  factory Counter() =>
      BridgeApiImpl.instance.counterNew();

  Future<void> increment({int delta = 1}) =>
      BridgeApiImpl.instance.counterIncrement(this, delta);

  Future<int> value() => BridgeApiImpl.instance.counterValue(this);

  Future<int> heavyCompute(int rounds) =>
      BridgeApiImpl.instance.counterHeavyCompute(this, rounds);

  static int sum(int a, int b) => BridgeApiImpl.instance.counterSum(a, b);

  // 返回自身类型 → 普通异步方法，不是 factory
  Future<Counter> duplicate() => BridgeApiImpl.instance.counterDuplicate(this);

  @override
  String toString() => BridgeApiImpl.instance.counterToString(this);
}
```

- 实例方法 payload 第一个字段为 handle，Dart 侧不暴露
- C++ 默认参数 → Dart 可选位置参数
- `BRIDGE_TO_STRING` → Dart `toString()` 覆写

---

## 大缓冲区：地址传递模式

普通的 `std::vector<uint8_t>` 会映射为 `Uint8List`，字节会被序列化进
bridge frame。对于大块 native 内存，应使用上面的 `uint8_t*` 特判，并把
长度作为独立参数传给 C++。此时 wire 仍然会传输地址和长度字段，但不会
复制地址指向的字节。

代码生成 demo 中有完整的同步和异步 round trip：
[`bridge_api.h`](https://github.com/deretame/dart_cpp_bridge/blob/main/examples/codegen_demo/native/api/bridge_api.h)
和
[`api_test.dart`](https://github.com/deretame/dart_cpp_bridge/blob/main/examples/codegen_demo/integration_test/api_test.dart)。

```cpp
// 头文件：Pointer<Uint8> 没有长度，所以长度必须进入 API。
BRIDGE_SYNC
std::uint8_t* echo_bytes(const std::uint8_t* data, std::int32_t len);

BRIDGE_ASYNC
stdexec::task<std::uint8_t*> async_echo_bytes(
    const std::uint8_t* data, std::int32_t len);
```

```cpp
// 实现：返回的地址必须在约定的生命周期内保持有效。
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

所有权是函数契约的一部分，不会体现在生成的类型中：

- Dart 分配的输入内存必须一直存活到同步调用返回，或异步 `Future`
  完成；
- C++ 必须自行校验地址和长度，bridge 无法验证任意 native 指针；
- 返回指针只在 C++ 文档约定的生命周期内有效。demo 返回的是 C++ 线程
  局部 buffer，不归 Dart 所有，不能传给 `calloc.free`；
- 地址只在当前进程内有效，不能持久化、跨进程传递，也不能假设跨 isolate
  有效。

普通数据优先使用 `std::vector<uint8_t>` → `Uint8List`，它更安全并且
具有值语义。

:::caution[注意]
只要 native 代码仍可能访问 buffer，就绝不能释放或复用它。指针不是能力
对象，也不是生命周期句柄；codegen 不会自动添加所有权和边界检查。
:::

---

## 不支持的类型

以下类型当前**不支持**，codegen 遇到时报错：

- 除 `std::uint8_t*` / `const std::uint8_t*` 之外的原始指针（如
  `int32_t*`、`char*`、`void*`）
- `std::unique_ptr<T>` 和 `std::shared_ptr<T>`
- 引用（`T&`）作为参数/返回值
- `std::variant`、`std::any`
- 位域、联合体
- 嵌套可空（`std::optional<std::optional<T>>`）
- 未特化模板、静态成员变量
