# Codegen 类型映射白名单

> 记录 dart_cpp_bridge Codegen 阶段当前计划支持的 C++ ↔ Dart 类型映射规则。用于后续实现 IR 生成、Dart 代码生成和 C++ wire 编解码时参照。
>
> 更新日期：2026-07-23
> 状态：基础类型、容器、Option、枚举、tuple、Stream、128 位整数、DartFn 已实现并测试；struct / opaque 类方法生成待做。

---

## 1. 基础类型映射

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `bool` | `bool` | 按 1 字节编码/解码 |
| `int8_t` / `i8` | `int` | 8 位有符号整数 |
| `int16_t` / `i16` | `int` | 16 位有符号整数 |
| `int32_t` / `i32` | `int` | 32 位有符号整数 |
| `int64_t` / `i64` | `int` | 64 位有符号整数（Dart `int` 在 native 上为 64 位） |
| `uint8_t` / `u8` | `int` | 8 位无符号整数 |
| `uint16_t` / `u16` | `int` | 16 位无符号整数 |
| `uint32_t` / `u32` | `int` | 32 位无符号整数 |
| `uint64_t` / `u64` | `int` | 64 位无符号整数（Dart `int` 在 native 上为 64 位） |
| `float` / `f32` | `double` | 32 位浮点数 |
| `double` / `f64` | `double` | 64 位浮点数 |
| `std::string` | `String` | UTF-8 字符串，wire 编码为长度 + 字节 |
| `char*` / `const char*` | `String` | 仅作为输入参数；不用于返回值或字段（按 `std::string` 处理） |

> 注：在 C++ 头文件里，可以写 `int8_t` / `int16_t` 等标准固定宽度类型，也可以用 `i8` / `i16` 等 FRB 风格别名（若用户定义了别名）。 codegen 解析时统一归一到标准类型处理。

---

## 2. 大整数

所有 128 位整数（包括 `__int128` / `unsigned __int128` 及其别名）在 wire 上**统一使用固定标记位 + 字符串**传输。本项目**不直接支持**编译器相关的 `__int128` / `unsigned __int128`，而是只提供统一的 `Int128` / `UInt128` 类型，内部以**字符串**存储数值，避免引入额外第三方库。

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `Int128` / `UInt128` | `BigInt` | 本项目统一使用的 128 位整数类型，内部以字符串形式存储，wire 上使用固定标记位 + 字符串传输 |

- 规则：宽度大于 64 位的整数，本项目**只允许**通过 `Int128` / `UInt128` 暴露给 Dart，映射为 `BigInt`。
- 编码：wire 中先使用一个**固定的标记位**标识大整数，随后传输字符串；不采用定长 16 字节 little-endian 整数。
- 原因：128 位整数不在 C++ 标准中，使用频率低，避免引入重量级三方库。
- 建议：如果业务需要真正 128 位运算，可在 C++ 侧自行使用 `boost::multiprecision::int128_t` 或编译器扩展，与 `Int128` / `UInt128` 的字符串表示相互转换。

---

## 3. 枚举

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `enum class T : int32_t` / `enum class T` | `enum T` | 枚举按底层整型编码 |
| `enum T` | `enum T` | 同上 |

- 枚举默认按 `int32_t` 在 wire 上传输。
- Dart 侧生成同名的 `enum T`，并为每个枚举值生成对应常量。
- 不支持位域枚举（`enum class` 带 `[Flags]` 等标记暂不支持，后续若需要再扩展）。

---

## 4. 容器与 Option

### 4.1 `std::vector<T>` / `std::array<T, N>`

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `std::vector<T>` / `std::array<T, N>` | `List<T>` | 普通元素列表，使用 Dart `List` |
| `std::vector<uint8_t>` / `std::array<uint8_t, N>` | `Uint8List` | 优先使用 typed list |
| `std::vector<uint16_t>` / `std::array<uint16_t, N>` | `Uint16List` | 优先使用 typed list |
| `std::vector<uint32_t>` / `std::array<uint32_t, N>` | `Uint32List` | 优先使用 typed list |
| `std::vector<uint64_t>` / `std::array<uint64_t, N>` | `Uint64List` | 优先使用 typed list |
| `std::vector<int8_t>` / `std::array<int8_t, N>` | `Int8List` | 优先使用 typed list |
| `std::vector<int16_t>` / `std::array<int16_t, N>` | `Int16List` | 优先使用 typed list |
| `std::vector<int32_t>` / `std::array<int32_t, N>` | `Int32List` | 优先使用 typed list |
| `std::vector<int64_t>` / `std::array<int64_t, N>` | `Int64List` | 优先使用 typed list |
| `std::vector<float>` / `std::array<float, N>` | `Float32List` | 优先使用 typed list |
| `std::vector<double>` / `std::array<double, N>` | `Float64List` | 优先使用 typed list |
| `std::vector<bool>` | `List<bool>` | 无 `BoolList`，回退到 `List<bool>` |

- 规则：`std::vector` 和 `std::array` 都映射为 Dart 的 `List`（或 typed list），长度由 payload 决定或数组大小固定。
- **优先使用 typed list**：当元素类型是固定宽度整数或浮点数时，Dart 侧生成 `Uint8List`、`Int32List` 等 typed list，避免装箱。
- 元素类型不支持循环引用；元素本身必须是本白名单内的类型。

### 4.2 `std::optional<T>`

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `std::optional<T>` | `T?` | 对应 Dart 的可空类型 |

#### 编码规则（统一使用 presence tag）

`std::optional<T>` 在 wire 上**不是**用 `T` 的某个特殊值（如 `0`、`null` 字符串）来表示空，而是显式先传一个 1 byte 的 presence tag，再条件传输 `T`：

```text
Some(T):  tag u8 = 1  +  T 的完整编码
None:     tag u8 = 0
```

这样即使 `T` 本身取到 `0`、空字符串、空列表等值，也不会和「无值」混淆。

#### C++ 侧实现

在 `ByteWriter` / `ByteReader` 中提供模板辅助，让 option 包装与具体类型解耦：

```cpp
// 写
w.opt<std::int32_t>(out, [&w](std::int32_t v) { w.i32(v); });
w.opt<std::string>(out, [&w](const std::string& s) { w.str(s); });

// 读
auto v = r.opt<std::int32_t>([&r]() { return r.i32(); });
auto s = r.opt<std::string>([&r]() { return r.str(); });
```

codegen 生成 wire 代码时，每个 `std::optional<T>` 对应调用上述模板，只需把 `T` 的基础编码函数传入即可，不需要为每种类型重复实现 option 逻辑。

#### Dart 侧实现

Dart 侧同样使用通用 helper，避免为每个类型手写：

```dart
T? decodeOpt<T>(T Function() decodeValue) {
  final hasValue = u8() != 0;
  if (!hasValue) return null;
  return decodeValue();
}

void encodeOpt<T>(T? value, void Function(T v) encodeValue) {
  if (value == null) {
    u8(0);
  } else {
    u8(1);
    encodeValue(value);
  }
}
```

生成示例：

```dart
final x = decodeOpt(() => r.i32());     // int?
encodeOpt(x, (v) => w.i32(v));

final name = decodeOpt(() => r.str());  // String?
encodeOpt(name, (v) => w.str(v));
```

#### 限制

- Dart 侧生成 `T?` 类型。
- 嵌套 `std::optional<std::optional<T>>` 暂不支持（与 Dart 类型语义不一致）。
- `T` 本身必须是本白名单支持的类型。

### 4.3 `std::unordered_map<K, V>`

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `std::unordered_map<K, V>` | `Map<K, V>` | 对应 Dart 的 `Map` |

- 编码：wire 中先传长度，再逐个传键值对。
- `K` 必须是不可变、可哈希的类型（目前支持基础类型、`enum`、字符串）。
- `V` 必须是本白名单内的类型。

### 4.4 `std::unordered_set<T>`

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `std::unordered_set<T>` | `Set<T>` | 对应 Dart 的 `Set` |

- 编码：wire 中先传长度，再逐个传元素。
- `T` 必须是不可变、可哈希的类型（基础类型、`enum`、字符串）。

### 4.5 `std::pair<T1, T2>` / `std::tuple<T1, T2, ...>`

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `std::pair<T1, T2>` | `(T1, T2)` | Dart 3 Record，位置对应 first/second |
| `std::tuple<T1, T2, ...>` | `(T1, T2, ...)` | Dart 3 Record，按位置一一对应 |

- 编码：wire 中按元素顺序依次编码，**不传输长度或字段名**（元素个数在编译期确定）。
- 元素类型必须是本白名单支持的类型。
- 由于 Dart Record 字段没有名字，C++ 端 `std::tuple` 也按位置映射，codegen 阶段不负责生成字段名。

---

## 5. 类与结构体

> 状态：数据类与 Opaque 类的**运行时基础设施和第一阶段手写测试已完成**；**代码生成（第二阶段）待实现**。本节把已验证的运行时行为和计划中的生成规则写在一起，作为后续 `parse_api.py` / `generate.py` 改动的设计依据。

### 5.1 总体分类

导出 `class` / `struct` 必须带 `BRIDGE_EXPORT` / `DCB_EXPORT`（或等价的 `[[bridge::export]]`）标记。Codegen 根据类体内容把它分成两类，**两类不能混用**：

| 类型 | 判定标准 | wire 传递方式 | 是否进注册表 | 是否可跨 Isolate |
|------|----------|---------------|--------------|------------------|
| **数据类（data class）** | 只有 public 非静态数据字段，**没有导出成员函数** | 按值编码 | 否 | 是 |
| **Opaque 类** | 带有至少一个导出成员函数（构造/析构/实例/静态方法） | 对象句柄 | 是（per-Session） | 否 |

- 未标记 `BRIDGE_EXPORT` 的类/结构体即使被 API 使用，也当作普通 C++ 类型处理，不生成 Dart 类。
- 数据类字段类型必须是白名单内类型；嵌套类型也必须是数据类。
- Opaque 类**不允许按值传递**：参数/返回值中出现 Opaque 类时必须以指针/引用/句柄形式出现，生成代码统一按句柄处理。

### 5.2 数据类（data class）

#### 5.2.1 导出与字段规则

- 只导出 `public` 的**非静态数据成员**。
- 不导出 `private` / `protected` 成员、友元声明、静态成员变量、成员函数。
- 字段类型必须是本白名单支持的类型：基础类型、枚举、容器、`std::optional<T>`、`std::pair` / `std::tuple`、另一个数据类。
- 字段名保持 C++ 原样转为 Dart 小驼峰（与函数参数命名规则一致）。

#### 5.2.2 编码规则

数据类在 wire 上不传输字段名、类型标签或长度，而是**按 C++ 头文件中的声明顺序**逐个字段编码/解码。

```cpp
struct BRIDGE_EXPORT Point {
    double x;
    double y;
};

// wire 布局：x (f64) + y (f64)
```

解码：

```cpp
Point p;
p.x = r.f64();
p.y = r.f64();
```

嵌套数据类：

```cpp
struct BRIDGE_EXPORT Rect {
    Point topLeft;
    Point bottomRight;
};

// wire 布局：topLeft.x + topLeft.y + bottomRight.x + bottomRight.y
```

要求：

- C++ 与 Dart 生成代码必须基于同一份 IR 的字段顺序生成，顺序错位会导致 wire 解析错误。
- 不支持循环引用；codegen 在 IR 阶段检测并报错。

#### 5.2.3 Dart 生成形态

为每个数据类生成不可变的 Dart 值类：

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
      other is Point &&
          runtimeType == other.runtimeType &&
          x == other.x &&
          y == other.y;
}
```

生成约定：

- 字段类型按白名单映射表转换（`std::optional<T>` → `T?`，容器 → typed list 等）。
- 不可空字段用 `required`，可空字段为可选命名参数。
- 字段顺序与 C++ 声明顺序一致。
- 生成 `hashCode` 和 `operator ==`（基于所有字段），方便 Dart 侧作为值类型使用。
- `toString` / `copyWith` 等语法糖当前阶段不做。

#### 5.2.4 C++ 生成形态

Codegen 为每个数据类生成两个内联函数（放在 `wire_dispatch.cpp` 中，避免污染业务头文件）：

```cpp
// 编码
inline void encode_Point(ByteWriter& w, const Point& v) {
    w.f64(v.x);
    w.f64(v.y);
}

// 解码
inline Point decode_Point(ByteReader& r) {
    Point v;
    v.x = r.f64();
    v.y = r.f64();
    return v;
}
```

生成规则：

- 函数名约定 `encode_<ClassName>` / `decode_<ClassName>`。
- 嵌套数据类递归调用对应 encode/decode。
- 这些函数仅在生成 wire 内部使用；业务代码仍然直接操作原始 C++ 类型。

### 5.3 Opaque 类

#### 5.3.1 导出规则

- 类本身必须带 `BRIDGE_EXPORT`。
- 导出的方法必须带通道标记：`BRIDGE_SYNC`、`BRIDGE_ASYNC`、`BRIDGE_NORMAL`。
- 构造函数和析构函数使用特殊标记：
  - `BRIDGE_CONSTRUCTOR` / `BRIDGE_DESTRUCTOR`（推荐）。
  - 或按约定识别：与类同名且无返回类型的成员函数视为构造函数；`~T()` 视为析构函数。标记优先，约定兜底。
- 不导出 `private` / `protected` 方法、友元函数、虚函数、纯虚函数、重载运算符。
- 不导出拷贝/移动构造函数（避免按值拷贝 Opaque 对象）。
- 静态方法与非静态方法都按同样方式标记。

#### 5.3.2 对象注册表（per-Session）

运行时已实现 `dcb::ObjectHandleRegistry`（`include/dart_cpp_bridge/object_handle.hpp`）：

- 存储形式：`std::unordered_map<session_id, SessionStore>`，每个 `SessionStore` 内含 `local_handle → (shared_ptr<void>, DropFn)`。
- 全局句柄：`session_id << 32 | local_handle`。
- Session 关闭时调用 `drop_all(session_id)`，释放该 Session 的所有对象。
- 不同 Session 的句柄空间隔离，Opaque 对象**不能跨 Isolate 共享**。

Codegen 不需要自己实现注册表，只需要调用已有 API：

```cpp
// 构造对象后注册，获得全局 handle
auto obj = std::make_shared<Counter>(initialValue);
auto handle = dcb::ObjectHandleRegistry::instance().insert(
    session_id,
    obj,
    [](std::shared_ptr<void>& p) { /* p 析构时自动调用 Counter 析构函数 */ });
```

#### 5.3.3 构造函数

C++ 头文件示例：

```cpp
class BRIDGE_EXPORT Counter {
 public:
    BRIDGE_CONSTRUCTOR
    Counter(std::int32_t initialValue);

    BRIDGE_CONSTRUCTOR
    static Counter zero();
};
```

Wire 生成（`Counter(std::int32_t)`）：

```cpp
case counter_ctor_id: {
    ByteReader r(frame.payload.data(), frame.payload.size());
    const auto initialValue = r.i32();
    auto obj = std::make_shared<Counter>(initialValue);
    const auto handle = dcb::ObjectHandleRegistry::instance().insert(
        session_id, obj, [](std::shared_ptr<void>&) {});
    ByteWriter w;
    w.u64(handle);
    post_ok(session, gen, req, method, w.raw());
    break;
}
```

Dart 生成：

```dart
class Counter extends CppOpaqueInterface {
  Counter._({required super.bridge, required super.handle});

  factory Counter({required int initialValue}) {
    return BridgeApi.instance._counterCtor(initialValue: initialValue);
  }

  factory Counter.zero() {
    return BridgeApi.instance._counterZero();
  }
}
```

- 工厂构造（static 方法返回 Opaque 对象）本质上与普通 static 方法相同，只是 Dart 侧把它暴露为 `factory Counter.zero()`。
- 默认构造函数：参数为空或全部带默认值的构造函数在 Dart 侧生成 `factory Counter()`。

#### 5.3.4 析构函数 / 生命周期

- C++ 析构函数不需要单独生成 wire method；运行时提供统一的 `dcb_drop_object(handle)`。
- Dart 侧 `CppOpaqueInterface.dispose()` 直接调用 `dcb_drop_object`。
- `NativeFinalizer` attach 到对象上，GC 时自动调用 `dcb_drop_object`。
- Session 关闭时注册表 `drop_all` 释放该 Session 全部对象，因此不需要 Dart 显式 dispose。

#### 5.3.5 实例方法

实例方法在 wire payload 中第一个字段是对象 handle，后面跟着普通参数。

C++ 头文件示例：

```cpp
class BRIDGE_EXPORT Counter {
 public:
    BRIDGE_ASYNC
    async_simple::coro::Lazy<std::int32_t> value() const;

    BRIDGE_SYNC
    void increment(std::int32_t delta = 1);
};
```

Wire 生成（async `value`）：

```cpp
case counter_value_id: {
    ByteReader r(frame.payload.data(), frame.payload.size());
    const auto handle = r.u64();
    auto obj = dcb::ObjectHandleRegistry::instance().get(handle);
    if (!obj) {
        post_err(session, gen, req, method,
                 "Counter handle not found or already dropped");
        break;
    }
    Runtime::instance().spawn_on_asio(
        [session, gen, req, method, handle, obj]() -> async_simple::coro::Lazy<> {
            try {
                auto* that = static_cast<Counter*>(obj.get());
                auto out = co_await that->value();
                ByteWriter w;
                w.i32(out);
                post_ok(session, gen, req, method, w.raw());
            } catch (const std::exception& e) {
                post_err(session, gen, req, method, e.what());
            } catch (...) {
                post_err(session, gen, req, method, "unknown");
            }
            co_return;
        });
    break;
}
```

- Sync 实例方法直接在 dispatch 线程执行；async/normal/stream 实例方法先把 handle 解析出来，再在对应调度器上调用方法。
- 无效 handle 统一返回 `"<Class> handle not found or already dropped"`。
- `const` 方法当前阶段不影响 wire，仅作为文档提示。

#### 5.3.6 静态方法

静态方法没有 `this`，payload 中不包含 handle，与普通顶层函数生成方式相同。区别在于 Dart 侧把它挂在类上：

```dart
class Counter extends CppOpaqueInterface {
  static int sum(int a, int b) => BridgeApi.instance._counterSum(a, b);
}
```

静态方法可以返回基础类型、容器、数据类，也可以返回新的 Opaque 对象（返回 handle）。

#### 5.3.7 Dart 生成形态

```dart
abstract base class CppOpaqueInterface implements Finalizable {
  CppOpaqueInterface({required DartCppBridge bridge, required int handle})
      : _bridge = bridge,
        _handle = handle {
    _finalizer = NativeFinalizer(_bridge.bindings.dropObject);
    _attachFinalizer();
  }

  final DartCppBridge _bridge;
  final int _handle;
  late final NativeFinalizer _finalizer;
  bool _disposed = false;

  void _attachFinalizer() {
    _finalizer.attach(
      this,
      Pointer.fromAddress(_handle).cast<Void>(),
      externalSize: 64,
    );
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _finalizer.detach(this);
    _bridge.bindings.dropObject(Pointer.fromAddress(_handle).cast<Void>());
  }
}

class Counter extends CppOpaqueInterface {
  Counter._({required super.bridge, required super.handle});

  factory Counter({required int initialValue}) =>
      BridgeApi.instance._counterCtor(initialValue: initialValue);

  factory Counter.zero() => BridgeApi.instance._counterZero();

  Future<int> value() => BridgeApi.instance._counterValue(_handle);

  void increment([int delta = 1]) =>
      BridgeApi.instance._counterIncrement(_handle, delta);

  static int sum(int a, int b) => BridgeApi.instance._counterSum(a, b);

  Future<String> callCallback(FutureOr<String> Function(String value) cb) =>
      BridgeApi.instance._counterCallDartFn(_handle, cb);
}
```

- `CppOpaqueInterface` 在 `dart/lib/src/bridge.dart` 或生成代码中提供；codegen 直接复用。
- 实例方法第一个参数为 `_handle`，Dart 侧不暴露给业务。
- 默认参数：C++ 方法带默认值时，Dart 侧生成可选位置参数（优先）或可选命名参数。

#### 5.3.8 限制

- 不支持 Opaque 类作为数据类字段（数据类中嵌入 Opaque 类型会在解析阶段报错）。
- 不支持 Opaque 类按值传递（参数/返回值中 Opaque 类型统一按 handle 处理）。
- 不支持多态继承：不能把 `class B : public A` 当作 `A` 来传参或返回。
- 不支持抽象类、虚函数、纯虚函数、重载运算符作为导出方法。
- 不支持方法重载（当前阶段；同名不同签名的方法需要用户手动改名，或后续通过 name mangling 支持）。
- 默认不加对象级锁；多线程并发调用同一对象的线程安全由业务代码保证。

### 5.4 实现阶段划分

为降低复杂度，类/结构体生成拆成两步：

1. **数据类生成**：先做 `Point` / `Rect` 的端到端测试，验证字段顺序、嵌套、与顶层函数的参数/返回值配合。
2. **Opaque 类生成**：再做 `Counter` 生成版，覆盖构造/析构/同步/异步/静态/DartFn/Stream 方法，并复用第一阶段手写的测试场景。

### 5.5 测试 fixture 规划

在 `examples/codegen_demo` 中新增：

- `native/api/point_rect.h`：
  - `Point { double x; double y; }`
  - `Rect { Point topLeft; Point bottomRight; }`
  - 顶层函数 `double distance(Point a, Point b)` 验证数据类作为参数。
  - 顶层函数 `Point scale(Point p, double factor)` 验证数据类作为返回值。
  - 顶层函数 `Rect boundingBox(List<Point> points)` 验证 `List<data class>`。

- `native/api/counter.h`（生成版 Counter）：
  - 构造函数 `Counter(int32_t initialValue)`。
  - 静态方法 `Counter zero()`。
  - 实例方法：`increment([int delta = 1])`、`value()` async、`valueSync()` sync、`sum(a, b)` static。
  - `Normal` 方法：例如 `heavyCompute(int rounds)` 在 thread pool 执行。
  - `Stream` 方法：例如 `tickStream(int count, int intervalMs)`。
  - `DartFn` 方法：例如 `greetDartFn(dcb::DartFn<std::string(std::string)> cb, std::string name)`。

## 6. DartFn 反向回调

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `dcb::DartFn<Ret(Args...)>` | `FutureOr<Ret> Function(Args...)` | FRB 风格的 Dart 闭包反向调用：C++ 异步等待 Dart 返回结果 |

- 使用方式：在 API 头文件中将参数声明为 `dcb::DartFn<Ret(Args...)>`，语法类似 `std::function`。例如：

  ```cpp
  BRIDGE_ASYNC
  async_simple::coro::Lazy<std::string> greet_dart_fn(
      dcb::DartFn<std::string(std::string)> callback, std::string name);
  ```

- Dart 侧生成代码会：
  1. 按实际参数/返回值类型生成 `FutureOr<Ret> Function(Args...)`；
  2. 调用 `bridge.registerDartFn(binaryClosure)` 注册一个二进制包装闭包，获得一个 `fn_id`；
  3. 把 `fn_id` 按参数顺序写入请求 payload；
  4. 调用 C++ 方法；
  5. 在 `try / finally` 中调用 `bridge.unregisterDartFn(fn_id)`，确保闭包在调用结束后被清理。

- C++ 侧生成代码会：
  1. 从 payload 中按参数顺序读出 `fn_id`；
  2. 构造 `dcb::DartFn<Signature>(session, generation, fn_id, encode, decode)`，其中 `encode` / `decode` 由生成代码按 `Ret` 和 `Args...` 的类型生成；
  3. 在业务方法中通过 `callback.callAsync(args...)` 异步调用 Dart 闭包并 `co_await` 结果。

- 限制：
  - 参数/返回值类型必须是当前白名单支持的类型（基础类型、枚举、容器、`std::optional<T>`、`Int128` / `UInt128`、数据类等）。
  - 同步阻塞版本 `callSync` 也可用，但如果在 `io_context` 线程上调用会阻塞事件循环，由业务代码自行决定。
  - Dart 闭包必须在 C++ 调用期间保持注册状态；生成代码通过 `try / finally` 保证生命周期正确。

## 7. 当前白名单（实现优先级）

下表按优先级和实现顺序记录当前计划支持的类型：

| 优先级 | 类型类别 | 状态 |
|--------|----------|------|
| P0 | 基础类型（bool、整型、浮点、string） | 已手写测试（Phase 1） |
| P0 | `std::vector<T>` / `std::array<T, N>` | 已手写测试（含 typed list 优化） |
| P0 | `std::optional<T>` | 已手写测试 |
| P1 | 枚举（enum / enum class） | 已手写测试 |
| P1 | `std::unordered_map<K, V>` | 已手写测试 |
| P1 | `std::unordered_set<T>` | 已手写测试 |
| P1 | `std::pair<T1, T2>` / `std::tuple<T1, T2, ...>` → Dart Record | 已手写测试 |
| P1 | 数据类（只有 public 字段的 struct / class，按值编解码） | 已手写测试；codegen 进行中 |
| P2 | 大整数 `Int128` / `UInt128` → `BigInt`（统一字符串存储） | 已手写测试 |
| P2 | Typed list 优化（`Vec<u8>` → `Uint8List` 等） | 已手写测试 |
| P3 | Opaque 类方法导出（构造/析构/实例/静态方法） | 已手写测试；codegen 待实现 |
| P3 | 嵌套复合类型（`list<struct>` 等） | 已手写测试 |

---

## 8. 明确不支持（当前阶段）

以下类型或特性当前白名单**不包含**，codegen 遇到时应报错或忽略：

- 指针类型（`T*`、`std::unique_ptr<T>`、`std::shared_ptr<T>` 等）
- 引用（`T&`）作为参数/返回值类型（wire 中按值传递）
- `std::map`、`std::set`（顺序容器）
- `std::variant`、`std::any`
- 位域（bit-field）
- 模板参数未特化的泛型类型
- 嵌套的可空（`std::optional<std::optional<T>>`）
- 静态成员变量
- 联合体（`union`）

---

## 9. 编码原则

1. **Dart 侧优先使用 typed list**：只要元素类型是固定宽度整数或浮点，就生成 `Uint8List`、`Int32List` 等 typed list，减少内存和装箱开销。
2. **可空优先使用 `?`**：`std::optional<T>` 映射为 `T?`，而不是 `Option<T>` 包装类。
3. **复合类型递归检查**：容器、类、option 的元素/字段类型必须是白名单内支持的类型，否则 codegen 报错。
4. **类型别名归一**：C++ 中 `i32`、`i64`、`f32` 等 FRB 风格别名，若被识别为 `int32_t`、`int64_t`、`float` 等基础类型，按基础类型处理。
5. **枚举按底层整型**：wire 中只传输整型值，不传输枚举名称。

---

## 相关文档

- [frb_and_cpp_bridge_design.md](./frb_and_cpp_bridge_design.md) — 整体架构与锁定决策
- [progress.md](./progress.md) — 实现进度
- [known_issues.md](./known_issues.md) — 已知问题与技术债
- `codegen/README.md` — Codegen 工具链说明
