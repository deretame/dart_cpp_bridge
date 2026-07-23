# Codegen 类型映射白名单

> 记录 dart_cpp_bridge Codegen 阶段当前计划支持的 C++ ↔ Dart 类型映射规则。用于后续实现 IR 生成、Dart 代码生成和 C++ wire 编解码时参照。
>
> 更新日期：2026-07-22
> 状态：文档已整理，代码实现尚未完成。

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

128 位整数不在 C++ 标准内，且不同编译器扩展（如 GCC/Clang 的 `__int128`）并不跨平台。本项目**不直接支持** `__int128` / `unsigned __int128`，而是提供统一的 `Int128` / `UInt128` 类型，内部以**字符串**存储数值，避免引入额外第三方库。

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `Int128` / `UInt128` | `BigInt` | 本项目统一使用的 128 位整数类型，内部以字符串形式存储，wire 上使用固定标记位 + 字符串传输 |

- 规则：宽度大于 64 位的整数，本项目只通过 `Int128` / `UInt128` 暴露给 Dart，映射为 `BigInt`。
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

---

## 5. 类与结构体

### 5.1 导出规则

- 标记为 `BRIDGE_EXPORT` / `DCB_EXPORT` 的 `class` 或 `struct` 才会进入 IR 生成。
- 未标记的类/结构体即使被 API 使用，也当作普通 C++ 类型处理（不生成 Dart 类）。
- 导出的类按用途分为两类：
  - **数据类（data class）**：只有 public 数据字段，**没有导出成员函数**。按值编码传递，**不进入对象注册表**，因此可以跨 Isolate 自由传递和嵌套。
  - **Opaque 类**：带有导出成员函数（标记为 `BRIDGE_SYNC` / `BRIDGE_ASYNC` 等）。通过对象句柄在 per-Session 注册表中存活，**不能跨 Isolate 共享**。
- 数据类字段的类型必须是本白名单支持的类型，也可以是另一个数据类（递归要求：嵌套的类也必须是纯数据类）。

### 5.2 字段/成员导出规则

- 仅导出 **public** 的 **非静态非函数** 成员。
- 不导出 `private` / `protected` 成员。
- 不导出 **友元（friend）** 声明。
- 不导出 **成员函数**（方法），即使它们是 public。
- 如果某个字段需要导出，其类型必须是本白名单支持的类型。

### 5.3 方法导出规则

- 结构体/类上的**成员函数默认不导出**。
- 如果某个成员函数需要暴露给 Dart，需要与顶层函数一样，使用 `BRIDGE_SYNC`、`BRIDGE_ASYNC`、`BRIDGE_NORMAL` 等标记。
- 成员函数在 Dart 侧生成逻辑待定：可能是生成扩展方法，或生成在类上，需要后续设计确认。此处先记录规则，不展开实现。

### 5.4 编码规则（按字段顺序顺序编解码）

类/结构体在 wire 上不额外传输字段名或类型信息，而是**按字段在 C++ 头文件中的声明顺序**逐个编码/解码每个字段。

**编码（C++ → wire）**：

```cpp
struct BRIDGE_EXPORT Point {
    double x;
    double y;
};

// wire 布局：x (f64) + y (f64)
// 即：w.f64(point.x); w.f64(point.y);
```

**解码（wire → C++）**：

```cpp
Point p;
p.x = r.f64();
p.y = r.f64();
```

规则：

- 字段顺序必须与 C++ 头文件中的声明顺序**严格一致**，否则编解码错位。
- 每个字段的编码方式由其自身类型决定：基础类型直接写/读，复合类型（`std::optional`、容器、嵌套类）递归调用对应类型的编解码。
- 不传输字段名，因此 C++ 和 Dart 生成代码必须基于同一份 IR 字段顺序生成。

### 5.5 Dart 类生成形态

Dart 侧生成与 C++ 结构体字段顺序一致的普通 Dart class：

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

- 字段类型按本白名单映射表转换（例如 `std::optional<T>` → `T?`）。
- 不可空字段用 `required`，可空字段为可选命名参数。
- 字段顺序与 C++ 声明顺序一致。
- 生成 `hashCode` 和 `operator ==`（基于所有字段），方便 Dart 侧作为值类型使用。
- 是否生成 `toString` / `copyWith` 等语法糖：当前阶段不做，后续可按需扩展。

### 5.6 嵌套与递归

类字段类型可以是本白名单支持的任意类型，包括其他类/结构体：

```cpp
struct BRIDGE_EXPORT Point { double x; double y; };
struct BRIDGE_EXPORT Rect { Point topLeft; Point bottomRight; };
```

wire 布局：

```text
Rect: topLeft.x (f64) + topLeft.y (f64) + bottomRight.x (f64) + bottomRight.y (f64)
```

- 不支持循环引用（`struct A { A* next; }` 或相互嵌套导致无限递归）。
- codegen 在 IR 阶段应检测循环依赖并报错。

### 5.7 映射示例

```cpp
struct BRIDGE_EXPORT Point {
    double x;
    double y;
};

class BRIDGE_EXPORT Rect {
public:
    Point topLeft;
    Point bottomRight;
};
```

Dart 侧生成：

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

class Rect {
  final Point topLeft;
  final Point bottomRight;

  const Rect({required this.topLeft, required this.bottomRight});

  @override
  int get hashCode => topLeft.hashCode ^ bottomRight.hashCode;

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is Rect &&
          runtimeType == other.runtimeType &&
          topLeft == other.topLeft &&
          bottomRight == other.bottomRight;
}
```

### 5.8 参考 FRB 实现

FRB 的类编解码也是按字段顺序生成：

- Rust 侧：`impl SseDecode for T` 中按字段顺序逐个调用 `<FieldType>::sse_decode`，最后构造结构体。
- Dart 侧：生成 `class T { final ... }`，并在 `sse_decode_T` 中按字段顺序逐个 decode，然后调用构造函数。

本项目采用相同的顺序编解码策略，只是 C++ 侧需要 codegen 生成每个导出类的 `encode` / `decode` 函数（或直接用 `ByteReader` / `ByteWriter` 内联到 wire dispatch 中）。

### 5.9 成员函数导出（P3 手写测试阶段）

#### 5.9.1 为什么不能当作普通函数

成员函数（method）和普通自由函数有本质区别：

1. **绑定到对象实例**：方法调用必须有 `this` / `self`，不能脱离对象存在。
2. **可修改对象状态**：`obj.increment(5)` 会改变 `obj` 的内部字段。
3. **需要生命周期管理**：对象创建后不能立即释放，Dart 侧对象被 GC 或显式 `dispose` 时，C++ 侧才能销毁对应实例。
4. **可能涉及线程安全/锁**：如果对象会被多线程访问，调用方法时需要加锁。

因此，成员函数导出**不是**简单地把对象按值编码成方法参数，而是需要一套"对象句柄 + 注册表 + 生命周期管理"机制。

#### 5.9.2 参考 FRB 的实现方式

参考 Flutter Rust Bridge 生成的代码（例如 `frb_generated.rs` / `frb_generated.io.dart`），FRB 处理成员函数的核心是 **`RustOpaque<T>` / opaque 对象**。

**Rust 侧**（来自 `frb_generated.rs`）：

```rust
fn wire__crate__api__http__HttpClient_base_url_impl(...) {
    // ...
    let api_that = <RustOpaque<HttpClient>>::sse_decode(&mut deserializer);
    // ...
    let api_that_guard = api_that.lockable_decode_sync_ref();
    let output_ok = crate::api::http::HttpClient::base_url(&*api_that_guard)?;
    // ...
}
```

要点：
- Dart 侧只持有一个不透明句柄（opaque handle）。
- 方法调用时，Dart 把句柄传回 Rust。
- Rust 根据句柄找到对象，获取引用（必要时加锁），再调用真实方法。
- 对象生命周期由引用计数或 `NativeFinalizer` 管理。

**Dart 侧**（来自 `frb_generated.io.dart`，概念示意）：

```dart
class HttpClient extends RustOpaque {
  String baseUrl() {
    return _apiImpl.httpClientBaseUrl(this);
  }
}
```

Dart 的 `HttpClient` 类只保存一个句柄，方法调用通过 FFI 转发到 Rust，Rust 再分派到真实成员函数。

#### 5.9.3 本项目最小实现设计

C++ 侧（运行时层）需要新增：

1. **对象注册表**：`std::unordered_map<std::uint64_t, std::shared_ptr<void>>` 或按类型区分的注册表，存储 `std::shared_ptr<T>`。
2. **对象句柄分配**：`std::uint64_t allocate_handle()`，单调递增。
3. **构造函数导出**：`BRIDGE_SYNC` / `BRIDGE_ASYNC` 标记的构造函数，创建对象并返回 handle。
4. **析构函数**：`dcb_drop_object(std::uint64_t handle)`，从注册表删除并释放对象。
5. **方法 dispatch**：成员函数 wire 方法的 payload 包含 `handle` + 其他参数，C++ 侧根据 handle 找到对象，调用方法。

Dart 侧生成形态（对齐 FRB 的 `RustOpaqueInterface`）：

```dart
// 统一由 codegen 生成的 opaque 基类。
abstract base class CppOpaqueInterface implements Finalizable {
  CppOpaqueInterface({required this._bridge, required this._handle}) {
    _finalizer = NativeFinalizer(_bridge._b.dropObject);
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
    _bridge._b.dropObject
        .asFunction<void Function(Pointer<Void>)>()(
          Pointer.fromAddress(_handle).cast<Void>(),
        );
  }
}

class Counter extends CppOpaqueInterface {
  Counter._({required super.bridge, required super.handle});

  factory Counter({required int initialValue}) {
    return DartCppBridge.instance.createCounter(initialValue: initialValue);
  }

  Future<void> increment(int delta) => _bridge._counterIncrement(_handle, delta);

  Future<int> value() => _bridge._counterGetValue(_handle);

  int valueSync() => _bridge._counterValueSync(_handle);

  static int sum(int a, int b) => DartCppBridge.instance._counterStaticSum(a, b);

  Future<String> callCallback(FutureOr<String> Function(String value) cb) =>
      _bridge._counterCallDartFn(_handle, cb);
}
```

要点：

- Dart 侧 opaque 类 **继承** `CppOpaqueInterface` 基类（手写测试 fixture 采用 `extends`；生成代码若需要继承其他类，可改为 `implements` 并自行重复 finalizer 逻辑）。
- `dispose()` 和 `NativeFinalizer` 的 attach/detach 逻辑在基类中统一实现，避免每个生成类重复。
- 构造函数在 Dart 侧生成 factory，内部调用 C++ 构造函数 wire 方法获得 handle。

#### 5.9.4 标记规则

- 只有标记了 `BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL` 的 public 成员函数才会导出。
- 未标记的成员函数不导出。
- 构造函数和析构函数需要特殊标记（如 `BRIDGE_CONSTRUCTOR` / `BRIDGE_DESTRUCTOR`），或通过约定识别（如 `T()` 作为构造函数）。
- 友元函数、private/protected 方法不导出。

#### 5.9.5 限制

- 当前阶段（P3）仍处于**手写测试阶段**，`Counter` fixture 已覆盖 async / sync / static / DartFn / Normal / Stream 成员方法，以及多实例独立、dispose 后错误等基础场景；但**代码生成尚未实现**，构造函数/析构函数的通用标记、默认参数、方法重载等也还未完成。当前首要目标是继续把手写测试跑通。
- 导出为 Dart 类的 `class` / `struct` **必须定义在 API 头文件**（codegen 扫描的用户头文件，如 `native/api/*.h`）中，否则 codegen 不会进入 IR。
- **不支持虚函数、纯虚函数、重载运算符**作为导出方法。
- **不支持多态继承**：不能把 `class B : public A` 当作 `A` 来传参或返回。
- **不支持抽象类**：导出的类必须有完整实现，不能是抽象基类。
- **Opaque 对象（带导出成员函数的类）不支持跨 Isolate 共享**：对象句柄注册表已实现为 **per-Session**，不同 Isolate 的 Session 看不到彼此的对象。句柄编码为 `session_id << 32 | local_handle`。
- **Opaque 对象不能按值返回**：成员函数返回 opaque 对象时必须返回 handle（Dart 侧为 `Counter` 等 opaque 类），不能按值拷贝；只有 trivially copyable 的数据类可以按值返回。
- 但**普通数据类 / 数据结构体**（只有 public 字段、没有导出方法）仍然可以跨 Isolate 使用，因为它们按值编码传递，不依赖句柄生命周期，也**不进入对象注册表**。
- **数据类可以嵌套其他数据类**，但嵌套的类也必须是纯数据类（只有 public 字段，没有导出方法）。codegen 在解析时应检查并拒绝在数据类中嵌入 opaque 类型。
- 暂不支持对象的方法在 Dart 侧被多线程并发调用时的默认加锁（由业务代码自己保证线程安全）。

#### 5.9.6 待完善清单

| 序号 | 完善项 | 说明 |
|------|--------|------|
| 1 | **Dart 侧 `CppOpaqueInterface` 基类** | 已实现：手写测试用 `extends` 复用 `dispose()` / `NativeFinalizer` attach/detach。 |
| 2 | **构造函数多种形态** | 已实现并手写测试：默认构造 `Counter.defaultCtor()`、带参构造 `Counter.create(initialValue)`、工厂构造 `Counter.zero()`。copy/move 构造为 codegen 阶段限制。 |
| 3 | **析构函数生命周期** | 已实现并手写测试：`dispose()` 手动释放 + `NativeFinalizer` 自动释放；Session 关闭时 per-Session 注册表自动 drop 所有对象。 |
| 4 | **Sync / Async / Normal / Stream 成员方法** | 已实现并手写测试：Counter 覆盖六种调用模式。 |
| 5 | **Static 方法** | 已实现并手写测试：`Counter.sum(a, b)`。 |
| 6 | **方法重载** | 同名不同参数的方法需要生成不同 method_id。 |
| 7 | **默认参数** | C++ 默认参数在 Dart 侧生成显式可选参数。 |
| 8 | **const 方法** | 标记为只读，不影响 wire，但可用于文档/代码提示。 |
| 9 | **丰富参数/返回值类型** | 支持基本类型、容器、option、枚举、其他 opaque 对象作为参数或返回值。 |
| 10 | **对象注册表改为 per-Session** | 已实现：句柄编码为 `session_id << 32 \| local_handle`，Session 关闭时自动 drop 该 Session 的对象。 |
| 11 | **对象方法线程安全** | 明确默认不加对象级锁，业务代码保证；或可选加锁策略。 |
| 12 | **无效句柄错误信息** | handle 不存在或已 drop 时返回清晰错误。 |
| 13 | **更多手写测试** | async/sync/static/DartFn/Normal/Stream 多实例等已测；剩余 GC 自动释放、跨 Isolate 句柄隔离等。 |

#### 5.9.7 第一阶段手写测试剩余目标

以下目标属于**第一阶段手写测试**（Phase 1），在正式启动 codegen 之前需要逐个跑通。按推荐顺序排列：

1. **构造函数多种形态** ✅
   - 默认构造：`Counter()`（等价于 `initialValue = 0`）。
   - 带参构造：已有 `Counter(initialValue)`，进一步验证参数校验路径。
   - 工厂构造（静态方法）：如 `Counter.zero()`，验证 static 方法返回 opaque 对象 handle。
   - 拷贝/移动构造限制：验证 opaque 对象不导出 copy/move constructor，禁止隐式按值拷贝。

2. **析构函数生命周期 / GC 自动释放** ✅
   - 明确 `dispose()` 手动释放与 `NativeFinalizer` 自动释放的语义。
   - 手写测试验证：bridge shutdown 时会关闭 Session，per-Session 注册表自动 drop 该 Session 的所有对象，旧 Counter 句柄失效。
   - GC 自动释放依赖 Dart `NativeFinalizer`，由 `CppOpaqueInterface` 统一 attach/detach；因 Dart GC 时机不可控，不手写确定性测试，但机制已跑通。

3. **无效句柄错误信息**
   - handle 不存在或已 drop 时返回清晰错误，区分“未找到”和“已释放”。
   - 例如：`Counter handle not found or already dropped in session X`。

4. **默认参数**
   - C++ 成员方法支持默认参数，Dart 侧生成显式可选命名参数。
   - 示例：`Counter.increment([int delta = 1])`，Dart 侧 `counter.increment()` 默认 `+1`。

5. **丰富的参数/返回值类型**
   - 扩展 Counter 或新增 fixture，验证成员方法参数/返回值可以是：
     - 基础类型、枚举、optional、容器（list/map/set）。
     - 其他数据类（按值传递）。
     - 其他 opaque 对象（返回 handle）。

6. **跨 Isolate 句柄隔离**
   - 验证 per-Session 注册表：在 worker isolate 创建的 Counter handle，传回 main isolate 后使用会失败。
   - 这是 per-Session 句柄设计的关键安全边界。

以上目标完成后，第一阶段手写测试基本闭环，再进入 codegen（第二阶段）。


## 6. 当前白名单（实现优先级）

下表按优先级和实现顺序记录当前计划支持的类型：

| 优先级 | 类型类别 | 状态 |
|--------|----------|------|
| P0 | 基础类型（bool、整型、浮点、string） | 已手写测试（Phase 1） |
| P0 | `std::vector<T>` / `std::array<T, N>` | 已手写测试（含 typed list 优化） |
| P0 | `std::optional<T>` | 已手写测试 |
| P1 | 枚举（enum / enum class） | 已手写测试 |
| P1 | `std::unordered_map<K, V>` | 已手写测试 |
| P1 | `std::unordered_set<T>` | 已手写测试 |
| P1 | 类/结构体（public 字段、自动导出、友元不导出） | 已手写测试 |
| P2 | 大整数 `Int128` / `UInt128` → `BigInt`（统一字符串存储） | 已手写测试 |
| P2 | Typed list 优化（`Vec<u8>` → `Uint8List` 等） | 已手写测试 |
| P3 | 类/结构体上的方法导出（需标记） | 手写测试阶段（Counter fixture 已覆盖 async/sync/static/DartFn/Normal/Stream，codegen 尚未实现） |
| P3 | 嵌套复合类型（`list<struct>` 等） | 已手写测试 |

---

## 7. 明确不支持（当前阶段）

以下类型或特性当前白名单**不包含**，codegen 遇到时应报错或忽略：

- 指针类型（`T*`、`std::unique_ptr<T>`、`std::shared_ptr<T>` 等）
- 引用（`T&`）作为参数/返回值类型（wire 中按值传递）
- `std::map`、`std::set`（顺序容器）
- `std::tuple`、`std::pair`（后续可能作为 `Record` 或自定义类支持）
- `std::variant`、`std::any`
- 位域（bit-field）
- 模板参数未特化的泛型类型
- 嵌套的可空（`std::optional<std::optional<T>>`）
- 静态成员变量
- 联合体（`union`）

---

## 8. 编码原则

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
