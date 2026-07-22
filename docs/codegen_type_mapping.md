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

当 C++ 整型宽度超过 64 位时，Dart 端使用 `BigInt`：

| C++ 类型 | Dart 类型 | 说明 |
|----------|-----------|------|
| `__int128` / `unsigned __int128` | `BigInt` | 128 位整数，wire 编码建议按 16 字节 little-endian 传输 |
| `int128_t` / `uint128_t`（若别名） | `BigInt` | 同上 |

- 规则：只要整型类型宽度大于 64 位，就映射为 `BigInt`。
- 编码：wire 中以定长字节数组传递，Dart 侧用 `BigInt` 解析/构造。

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

---

## 6. 当前白名单（实现优先级）

下表按优先级和实现顺序记录当前计划支持的类型：

| 优先级 | 类型类别 | 状态 |
|--------|----------|------|
| P0 | 基础类型（bool、整型、浮点、string） | 待实现 |
| P0 | `std::vector<T>` / `std::array<T, N>` | 待实现 |
| P0 | `std::optional<T>` | 待实现 |
| P1 | 枚举（enum / enum class） | 待实现 |
| P1 | `std::unordered_map<K, V>` | 待实现 |
| P1 | `std::unordered_set<T>` | 待实现 |
| P1 | 类/结构体（public 字段、自动导出、友元不导出） | 待实现 |
| P2 | 大整数 `__int128` / `unsigned __int128` → `BigInt` | 待实现 |
| P2 | Typed list 优化（`Vec<u8>` → `Uint8List` 等） | 待实现 |
| P3 | 类/结构体上的方法导出（需标记） | 待设计/待实现 |

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
