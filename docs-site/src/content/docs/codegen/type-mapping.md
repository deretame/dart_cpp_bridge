---
title: 类型映射
description: C++ ↔ Dart 类型映射规则，包括数据类与不透明类
sidebar:
  order: 3
---

## 基础类型

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `bool` | `bool` | 1 字节编码 |
| `int8_t` / `int16_t` / `int32_t` / `int64_t` | `int` | 有符号整数 |
| `uint8_t` / `uint16_t` / `uint32_t` / `uint64_t` | `int` | 无符号整数 |
| `float` | `double` | 32 位浮点 |
| `double` | `double` | 64 位浮点 |
| `std::string` | `String` | 按字节原样传输，不以 `\0` 截断 |
| `std::chrono::system_clock::time_point` | `DateTime` | i64 Unix 微秒时间戳，不带时区信息 |
| `Int128` / `UInt128` | `BigInt` | 仅为标记用，不可实际使用 |

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
| `std::vector<int32_t>` | `Int32List` |
| `std::vector<float>` | `Float32List` |
| `std::vector<double>` | `Float64List` |
| `std::vector<T>`（其他） | `List<T>` |

固定宽度整数/浮点元素**优先使用 typed list**（`Uint8List`、`Int32List` 等），避免装箱。`std::vector<bool>` 回退为 `List<bool>`。

### std::optional → 可空类型

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `std::optional<T>` | `T?` |

Wire 编码使用 presence tag：`Some(T)` = tag 1 + T 编码，`None` = tag 0。

### std::unordered_map / std::unordered_set

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `std::unordered_map<K, V>` | `Map<K, V>` |
| `std::unordered_set<T>` | `Set<T>` |

### std::pair / std::tuple → Dart Record

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `std::pair<T1, T2>` | `(T1, T2)` |
| `std::tuple<T1, T2, ...>` | `(T1, T2, ...)` |

按位置一一对应，wire 中按元素顺序编码，不传长度或字段名。

## DartFn 反向回调

| C++ 类型 | Dart 类型 |
|----------|-----------|
| `dcb::DartFn<Ret(Args...)>` | `Future<Ret> Function(Args...)` |

支持任意数量参数，Dart 侧生成对应的多参数闭包。

### 异步调用（仿函数 operator()）

```cpp
// 在协程中通过 co_await 调用 Dart 闭包，不阻塞 io 线程
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> greet_dart_fn(
    dcb::DartFn<std::string(std::string)> callback, std::string name);
```

```cpp
// 实现：DartFn 是仿函数，operator() 返回 Lazy<Ret>
auto reply = co_await callback(name);
co_return "hello, " + reply;
```

### 阻塞调用（syncAwait）

```cpp
// 阻塞当前线程直到 Dart 回复 — 必须在线程池中使用（BRIDGE_NORMAL）
BRIDGE_NORMAL
std::string concat_dart_fn(
    dcb::DartFn<std::string(std::string, std::string)> callback,
    std::string a, std::string b);
```

```cpp
// 实现：通过 syncAwait + spawn 阻塞等待
auto reply = async_simple::coro::syncAwait(dcb::spawn(callback(a, b)));
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
- `syncAwait(dcb::spawn(fn(args...)))`：阻塞调用线程直到 Dart 回复，**必须在 `BRIDGE_NORMAL`（线程池）中使用**，禁止在 io 线程调用
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

// 触发方式 A：BRIDGE_NORMAL（线程池），通过 syncAwait 调用已存储的闭包
BRIDGE_NORMAL
std::string invoke_registered(std::string input);

// 触发方式 B：BRIDGE_ASYNC（协程），通过 co_await fn(...) 调用，不阻塞 io 线程
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> invoke_registered_async(std::string input);
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
  auto reply = async_simple::coro::syncAwait(dcb::spawn(g_registered_fn(input)));  // 安全：跑在线程池
  return "registered:" + reply;
}

async_simple::coro::Lazy<std::string> invoke_registered_async(std::string input) {
  if (!g_registered_fn) throw std::runtime_error("no registered dart fn");
  auto reply = co_await g_registered_fn(input);  // 协程挂起，io 不阻塞
  co_return "async_registered:" + reply;
}
```

```dart
// Dart 侧使用
final ok = registerDartFn(callback: (s) => 'echo:$s');  // 同步，只存储

// 方式 A：线程池 syncAwait
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
| `invokeRegistered()` | 线程池（BRIDGE_NORMAL） | 是，`syncAwait` 阻塞 pool 线程 |
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
#include <async_simple/coro/Lazy.h>

namespace demo::api {

// BRIDGE_SYNC — 在 io 线程同步执行，立即返回结果
// 适合：纯计算、无阻塞、微秒级操作
BRIDGE_SYNC
std::int32_t bridge_version();

// BRIDGE_ASYNC — C++20 协程，在 io 线程调度，可 co_await 挂起
// 适合：异步 IO、协程组合、需要等待其他异步操作
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);

// BRIDGE_NORMAL — 普通函数，投递到线程池执行
// 适合：CPU 密集计算、阻塞式文件/网络 IO、任何会阻塞的操作
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

}  // namespace demo::api
```

### 规则

- `BRIDGE_SYNC`：返回值直接编码回传，Dart 侧为同步调用（返回 `T`）
- `BRIDGE_ASYNC`：返回类型必须是 `async_simple::coro::Lazy<T>`，Dart 侧为 `Future<T>`
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

`bool`、`int8/16/32/64_t`、`uint8/16/32/64_t`、`float`、`double`、`std::string`、`std::chrono::system_clock::time_point`

示例：`std::string name;`

#### 枚举

`enum class T : std::int32_t`（必须标记 `BRIDGE_EXPORT`）

示例：`enum class Color : std::int32_t { kRed = 0, kGreen = 1, kBlue = 2 };`

#### 容器

`std::vector<T>`、`std::array<T, N>`、`std::optional<T>`、`std::unordered_map<K, V>`、`std::unordered_set<T>`、`std::pair<T1, T2>`、`std::tuple<T1, ...>`

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
- `Int128` / `UInt128` 仅作为标记存在，**不可实际用于字段**。
- 字段不支持指针、引用、不透明类、原始 C 数组、位域、联合体、`std::variant`、`std::any` 等。
- 容器元素也必须是白名单内类型（例如 `std::vector<AnotherDataClass>` 合法，`std::vector<std::unique_ptr<T>>` 不合法）。
- 不支持 `std::optional<std::optional<T>>` 等嵌套可空。

### Wire 编码

按 C++ 头文件中的**声明顺序**逐字段编码，不传字段名：

```text
Point  → x (f64) + y (f64)
Rect   → topLeft.x + topLeft.y + bottomRight.x + bottomRight.y
```

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
    async_simple::coro::Lazy<std::int32_t> value() const;

    // 线程池方法
    BRIDGE_NORMAL
    std::int64_t heavy_compute(std::int32_t rounds);

    // 静态方法
    BRIDGE_SYNC
    static std::int32_t sum(std::int32_t a, std::int32_t b);

    // 返回自身类型的方法 — 这不是工厂，只是普通异步方法
    BRIDGE_ASYNC
    async_simple::coro::Lazy<Counter> duplicate() const;

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

dart_cpp_bridge 的 wire 协议本身**不是零拷贝**的，帧数据需要在 Dart 与 C++ 之间做序列化/反序列化。不过对于日常类型（基础类型、小数据类、短字符串）来说，这份拷贝开销极小，基本不需要关心。

当你需要读写大段原始内存（图片、音频采样、大数组等）时，推荐在 Dart 侧分配 native 内存，然后只把**地址和长度**两个整数传给 C++：

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
  // 只传递两个 int64，消息拷贝开销可忽略
  final (outAddr, outLen, checksum) = await processBuffer(
    address: buffer.address,
    length: bufferSize,
  );
  // 如需读取 C++ 输出的内存，继续用 outAddr/outLen 访问
} finally {
  calloc.free(buffer);
}
```

这种方式的好处：

- 真正的大缓冲区**不会走消息通道**，Dart 和 C++ 通过同一段 native 内存协作
- wire 上只传递两个 `int64`，拷贝开销几乎可以忽略
- 异步代码写起来和普通函数一样，codegen 自动处理 FFI 调用和端口回调

:::caution[注意]
Dart 分配的内存需要自己管理生命周期，记得在合适的时机 `calloc.free(buffer)`。C++ 侧不应该在 Dart 已经释放内存之后继续访问该地址。
:::

---

## 不支持的类型

以下类型当前**不支持**，codegen 遇到时报错：

- 指针（`T*`、`std::unique_ptr<T>`、`std::shared_ptr<T>`）
- 引用（`T&`）作为参数/返回值
- `std::map`、`std::set`（有序容器）
- `std::variant`、`std::any`
- 位域、联合体
- 嵌套可空（`std::optional<std::optional<T>>`）
- 未特化模板、静态成员变量
