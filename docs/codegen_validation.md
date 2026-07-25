# Codegen 校验与错误处理规格

> 本文档定义 dart_cpp_bridge codegen 阶段应执行的所有静态校验规则、运行时错误传播机制，以及对应的测试用例矩阵。
>
> 更新日期：2026-07-25
> 状态：规格定义完成，部分规则已实现（标注 ✅），其余待实现（标注 ⏳）。

---

## 1. 概述

Codegen 校验分两个阶段：

| 阶段 | 时机 | 职责 | 失败行为 |
|------|------|------|----------|
| **静态校验** | `parse_api.py` 解析后、`generate.py` 生成前 | 检查导出 API 是否全部在白名单内 | `SystemExit`，打印错误列表，不生成任何代码 |
| **运行时传播** | 生成的 `wire_dispatch.cpp` 执行期间 | 捕获 C++ 业务异常，编码为错误帧发回 Dart | Dart 侧抛出 `StateError(message)` |

设计原则：

- **尽早失败**：能在 codegen 阶段发现的错误，不留到编译期或运行时。
- **定位精确**：每条错误携带 `文件名:行号`（如 `bridge_api.h:42`）。
- **提示可操作**：错误信息包含修复建议或文档链接。
- **不遗漏**：一次运行报告所有错误，而非遇到第一个就停止。

---

## 2. 静态校验规则

### 2.1 函数级校验（F 系列）

适用于所有带 `BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL` 标记的顶层函数和类方法。

| 规则 ID | 触发条件 | 状态 | 错误信息模板 |
|---------|----------|------|--------------|
| **F01** | 参数或返回值的类型不在白名单内（如 `std::filesystem::path`、`FILE*`、`void*`、`std::mutex`） | ✅ | `function \`{qname}\`, arg \`{arg}\`: unsupported type \`{spelling}\` at {loc}` |
| **F02** | 参数或返回值引用了未标记 `BRIDGE_EXPORT` 的类（既不是 data_class 也不是 opaque_class） | ✅ | 同 F01（当前落入 `unsupported`），Hint 中提示"该类未标记导出" |
| **F03** | 容器泛型参数含不支持类型（如 `vector<std::filesystem::path>`、`optional<FILE*>`） | ✅ | 递归报出内层 unsupported 类型 |

**修复建议模板**：
```
Hint: only types in the whitelist are allowed (primitives, std::string,
containers, std::optional, std::pair/tuple, enums, data classes, DartFn,
Int128/UInt128). See docs/codegen_type_mapping.md for the full whitelist.
```

**F02 增强（⏳ 待实现）**：当 `unsupported` 的 spelling 对应一个在扫描头文件中存在但未标记 `BRIDGE_EXPORT` 的 class/struct 时，Hint 应改为：
```
Hint: class `{name}` is not marked with BRIDGE_EXPORT. Add BRIDGE_EXPORT
to the class declaration, or remove it from the exported API surface.
```

---

### 2.2 类级校验（C 系列）

适用于所有带 `BRIDGE_EXPORT` / `DCB_EXPORT` 标记的 class/struct。

| 规则 ID | 触发条件 | 状态 | 错误信息模板 |
|---------|----------|------|--------------|
| **C01** | 导出类含有虚函数（`virtual` 方法或 override） | ⏳ | `class \`{name}\` at {loc}: exported class has virtual method \`{method}\`. Virtual dispatch is not supported across the bridge.` |
| **C02** | 导出类有继承关系（有 base class，无论是 `public`/`protected`/`private`） | ⏳ | `class \`{name}\` at {loc}: exported class has base class \`{base}\`. Inheritance is not supported for exported classes.` |
| **C03** | 导出类是模板类（`template<typename T> struct Foo`） | ⏳ | `class \`{name}\` at {loc}: exported class is a template. Template classes cannot be exported; use explicit non-template wrappers.` |
| **C04** | data_class 字段含不支持类型（包括未标记导出的类、指针、引用等） | ✅ | `data_class \`{name}\`, field \`{field}\`: unsupported type \`{spelling}\` at {loc}` |
| **C05** | data_class 字段按值嵌入 opaque_class | ✅ | `data_class \`{name}\`, field \`{field}\`: data class field cannot reference opaque class \`{opaque}\`. Opaque classes are handle-only.` |
| **C06** | data_class 字段含非白名单容器的泛型实例（如 `std::shared_lock<std::mutex>`、`boost::optional<int>`） | ✅ | 落入 F01/C04 的 `unsupported` 路径 |
| **C07** | opaque_class 方法的参数或返回值含不支持类型 | ✅ | `opaque_class \`{name}\`, method \`{method}\`, arg \`{arg}\`: unsupported type ...` |

#### C01 详细说明

检测方式：在 `_collect_classes` 遍历 `CXX_METHOD` 子节点时，检查 `cursor.is_virtual_method()` 或 `cursor.is_pure_virtual_method()`。

- 即使虚函数没有标记 `BRIDGE_SYNC` 等通道标记，只要类中有虚函数就报错。
- 原因：含虚函数的类有 vtable，按值编解码会破坏对象布局；opaque 类通过 `static_cast` 调用方法，多态行为不可预期。
- 析构函数 `virtual ~Foo()` 也算虚函数，应报错。

#### C02 详细说明

检测方式：在 `_collect_classes` 中检查 `cursor.get_children()` 中是否存在 `CursorKind.CXX_BASE_SPECIFIER`。

- 任何继承都报错，包括 `public`、`protected`、`private`、虚继承。
- 原因：继承关系改变对象内存布局，codegen 按声明字段顺序编解码，无法处理基类字段。

#### C03 详细说明

检测方式：在 `_collect_classes` 中检查 `cursor.kind == CursorKind.CLASS_TEMPLATE`（而非 `CLASS_DECL`/`STRUCT_DECL`）。

- 当前 `_collect_classes` 只匹配 `CLASS_DECL` / `STRUCT_DECL`，模板类不会被收集。
- 需要额外扫描 `CLASS_TEMPLATE`，若带 `BRIDGE_EXPORT` 标记则报错。
- 原因：codegen 为每个导出类生成固定的 encode/decode 函数，无法处理未特化的模板参数。

---

### 2.3 泛型/容器规则（G 系列）

| 规则 ID | 描述 | 状态 |
|---------|------|------|
| **G01** | 仅以下容器/包装器支持泛型参数 | ✅（隐式，由 `_type_ir` 结构决定） |
| **G02** | 容器的泛型参数必须递归满足白名单 | ✅（`_validate_ir` 递归检查） |
| **G03** | 容器可以无限嵌套（`vector<vector<vector<i32>>>` 合法） | ✅（递归解析无深度限制） |
| **G04** | 导出类本身不能是模板类 | ⏳（同 C03） |
| **G05** | 非白名单容器的泛型实例报 unsupported | ✅（落入 `_type_ir` 末尾 fallback） |

#### G01 白名单容器列表

以下模板名在 `_type_ir` 中被识别为合法容器：

| 模板名 | IR kind | 参数数量 | 说明 |
|--------|---------|----------|------|
| `std::vector` | `vector` | 1 | 动态数组 |
| `std::array` | `array` | 2（类型 + 大小） | 固定大小数组 |
| `std::optional` | `optional` | 1 | 可空 |
| `std::unordered_map` | `map` | 2（key + value） | 哈希表 |
| `std::unordered_set` | `set` | 1 | 哈希集合 |
| `std::pair` | `pair` | 2 | 二元组 |
| `std::tuple` | `tuple` | N | 多元组 |
| `async_simple::coro::Lazy` | `lazy` | 1 | 协程返回值包装 |
| `StreamSink` / `dcb::StreamSink` | `stream_sink` | 1 | 流事件接收器 |
| `dcb::DartFn` / `DartFn` | `dart_fn` | 1（函数签名） | Dart 回调 |

**不在上述列表中的任何模板实例**（如 `std::shared_ptr<T>`、`std::unique_ptr<T>`、`std::map<K,V>`、`std::set<T>`、`std::variant<T...>`、`boost::optional<T>`、`std::shared_lock<M>`）均落入 `unsupported`。

> 注：`std::shared_ptr` / `std::unique_ptr` 在 `codegen_type_mapping.md` §8 中明确列为"当前不支持"。

#### G02 递归白名单

容器内层参数允许的类型（递归定义）：

```
whitelist_type :=
  | primitive (bool, i8..i64, u8..u64, f32, f64, string)
  | i128 / u128
  | enum
  | data_class
  | opaque_class (仅作为函数参数/返回值的 handle 引用，不能作为 data_class 字段)
  | vector<whitelist_type>
  | array<whitelist_type, N>
  | optional<whitelist_type>
  | map<whitelist_type, whitelist_type>
  | set<whitelist_type>
  | pair<whitelist_type, whitelist_type>
  | tuple<whitelist_type, ...>
  | dart_fn<whitelist_type(whitelist_type, ...)>
```

#### G03 嵌套深度

- codegen 和 codec 对容器嵌套深度**不设硬限制**。
- `vector<vector<vector<i32>>>` → Dart `List<List<List<int>>>` 合法。
- `optional<vector<optional<string>>>` → Dart `List<String?>?` 合法。
- 实际限制来自 C++ 编译器模板递归深度和 wire payload 大小。
- **测试要求**：至少验证 3 层嵌套（见 §4 测试矩阵 G03-T1）。

#### G05 非白名单泛型示例

```cpp
// 以下全部报 unsupported：
std::shared_ptr<Counter>       // 指针类型
std::unique_ptr<Widget>        // 指针类型
std::map<std::string, int>     // 顺序 map（只支持 unordered_map）
std::set<int>                  // 顺序 set（只支持 unordered_set）
std::variant<int, std::string> // variant
std::any                       // any
boost::optional<int>           // 非 std 容器
std::filesystem::path          // 非容器，非白名单类型
```

---

### 2.4 Enum 校验（E 系列）

| 规则 ID | 描述 | 状态 |
|---------|------|------|
| **E01** | enum 底层类型必须是整型 | ✅（`_collect_enums` 中检查） |
| **E02** | 匿名 enum 跳过（不导出） | ✅（无 spelling 时 skip） |

当前实现已足够，无需额外校验。

---

### 2.5 校验执行流程

```text
parse_project()
  ├─ Pass 1: _collect_enums()
  ├─ Pass 2: _collect_classes()
  │    ├─ [⏳] C01: 检查虚函数
  │    ├─ [⏳] C02: 检查继承
  │    └─ [⏳] C03: 检查模板类
  ├─ _resolve_class_field_types()
  ├─ Pass 3: _collect_functions()
  └─ _validate_ir()
       ├─ [✅] F01/F02/F03: 函数参数/返回值
       ├─ [✅] C04/C05/C06/C07: 类字段/方法
       └─ [✅] G02: 容器递归白名单
```

所有错误收集完毕后一次性输出，格式：

```
============================================================
CODEGEN TYPE ERROR: unsupported types found in exported API
============================================================

  function `demo::api::read_file`, arg `path`: unsupported type `std::filesystem::path` at bridge_api.h:42
    Hint: only types in the whitelist are allowed ...

  class `demo::api::Config`, field `mutex`: unsupported type `std::mutex` at config.h:15
    Hint: ...

Found 2 type error(s). Fix the highlighted declarations and re-run codegen.
See docs/codegen_type_mapping.md for the full whitelist.
============================================================
```

---

## 3. 运行时错误传播

### 3.1 机制概述

```text
C++ 业务代码 throw
       ↓
wire_dispatch.cpp try/catch 捕获
       ↓
post_err(session, gen, req, method, message)
       ↓
编码为 responseErr 帧: [code i32=1] + [message string]
       ↓
Dart 侧 ReceivePort 收到帧
       ↓
async: Completer.completeError(StateError(message))
sync:  直接 throw StateError(message)
stream: StreamController.addError(StateError(message))
```

### 3.2 生成的 C++ 错误捕获代码

每个 dispatch case 内部结构：

```cpp
case method_id: {
    // ... decode args ...
    try {
        auto result = business_function(args...);
        // ... encode result ...
        post_ok(session, gen, req, method, payload);
    } catch (const std::exception& e) {
        post_err(session, gen, req, method, e.what());
    } catch (...) {
        post_err(session, gen, req, method, "unknown");
    }
    break;
}
```

对于 async（Lazy）方法，`try/catch` 包裹在协程 lambda 内部。

### 3.3 Dart 侧错误接收

| 通道 | Dart 行为 | 代码位置 |
|------|-----------|----------|
| async | `Future` 以 `StateError(message)` 完成 | `bridge.dart` `_handleFrame` → `completeError` |
| sync | 调用点直接 `throw StateError(message)` | `bridge.dart` `invokeSyncMethod` |
| normal | 同 async | 同 async |
| stream | `StreamController.addError(StateError(message))`，随后 `streamErr` 帧关闭流 | `bridge.dart` `_handleFrame` |

### 3.4 错误帧格式

```text
msg_type = responseErr (0x03) 或 streamErr (0x06)
payload:
  code    i32    固定为 1（通用错误码，未来可扩展）
  message string UTF-8 错误描述
```

### 3.5 特殊运行时错误

| 场景 | 行为 |
|------|------|
| Opaque 对象 handle 无效（已 drop 或不存在） | `post_err(..., "{Class} handle not found or already dropped")` |
| Dart 侧对已 dispose 的 Opaque 对象调用方法 | Dart 侧直接 `throw StateError("{Class} disposed")`，不发 FFI 请求 |
| 帧解析失败（magic/version/length 不匹配） | `post_err(..., "bad frame: {detail}")` |
| 未知 method_id | `post_err(..., "unknown method {id}")` |

---

## 4. 测试用例矩阵

### 4.1 静态校验测试（Codegen 阶段）

每条规则对应一个**反例**（应触发报错）和一个**正例**（应正常通过）。

#### F01: 不支持类型作为参数/返回值

**反例** `native/api/bad_types.h`：
```cpp
#pragma once
#include <dart_cpp_bridge/annotate.h>
#include <filesystem>
#include <cstdio>

BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> read_file(std::filesystem::path p);
// 期望报错: unsupported type `std::filesystem::path`

BRIDGE_SYNC
std::int32_t file_size(FILE* fp);
// 期望报错: unsupported type `FILE *`

BRIDGE_SYNC
void* get_raw_ptr();
// 期望报错: unsupported type `void *`
```

**正例**：
```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::string> read_file(std::string path);
// 通过：std::string 在白名单内
```

#### F02: 未标记导出的类

**反例**：
```cpp
// 注意：InternalConfig 没有 BRIDGE_EXPORT
struct InternalConfig {
    int timeout;
    std::string name;
};

BRIDGE_SYNC
InternalConfig get_config();
// 期望报错: unsupported type `InternalConfig`
// 增强 Hint: class `InternalConfig` is not marked with BRIDGE_EXPORT
```

**正例**：
```cpp
struct BRIDGE_EXPORT PublicConfig {
    std::int32_t timeout;
    std::string name;
};

BRIDGE_SYNC
PublicConfig get_config();
// 通过：PublicConfig 已标记导出，是 data_class
```

#### F03: 容器内含不支持类型

**反例**：
```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::vector<std::filesystem::path>> list_files();
// 期望报错: unsupported type `std::filesystem::path`（递归检出）

BRIDGE_SYNC
std::optional<FILE*> maybe_file();
// 期望报错: unsupported type `FILE *`
```

**正例**：
```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::vector<std::string>> list_files();
// 通过
```

#### C01: 导出类有虚函数

**反例**：
```cpp
class BRIDGE_EXPORT Shape {
public:
    virtual double area() const = 0;  // 纯虚函数
    virtual ~Shape() = default;       // 虚析构
};
// 期望报错: exported class has virtual method `area`
// 期望报错: exported class has virtual method `~Shape`
```

**正例**：
```cpp
struct BRIDGE_EXPORT Circle {
    double radius;
};
// 通过：无虚函数
```

#### C02: 导出类有继承

**反例**：
```cpp
struct Base { int x; };

struct BRIDGE_EXPORT Derived : public Base {
    int y;
};
// 期望报错: exported class has base class `Base`
```

**正例**：
```cpp
struct BRIDGE_EXPORT Standalone {
    std::int32_t x;
    std::int32_t y;
};
// 通过：无继承
```

#### C03: 导出模板类

**反例**：
```cpp
template <typename T>
struct BRIDGE_EXPORT Container {
    T value;
};
// 期望报错: exported class is a template
```

**正例**：
```cpp
struct BRIDGE_EXPORT IntContainer {
    std::int32_t value;
};
// 通过：非模板
```

#### C04: data_class 字段含不支持类型

**反例**：
```cpp
struct BRIDGE_EXPORT BadData {
    std::mutex mtx;           // 不支持
    std::filesystem::path p;  // 不支持
};
// 期望报错: unsupported type `std::mutex`
// 期望报错: unsupported type `std::filesystem::path`
```

**正例**：
```cpp
struct BRIDGE_EXPORT GoodData {
    std::int32_t id;
    std::string name;
    std::vector<double> values;
    std::optional<std::string> note;
};
// 通过
```

#### C05: data_class 字段嵌入 opaque_class

**反例**：
```cpp
class BRIDGE_EXPORT Engine {
public:
    BRIDGE_CONSTRUCTOR
    Engine();
    BRIDGE_ASYNC
    async_simple::coro::Lazy<void> start();
};

struct BRIDGE_EXPORT Car {
    Engine engine;  // 错误：opaque 类不能按值嵌入
    std::string model;
};
// 期望报错: data class field cannot reference opaque class `Engine`
```

**正例**：
```cpp
struct BRIDGE_EXPORT CarInfo {
    std::string model;
    std::int32_t horsepower;
};
// 通过：只含白名单类型
```

#### G03: 容器深层嵌套

**正例**（应正常通过并生成正确代码）：
```cpp
BRIDGE_SYNC
std::vector<std::vector<std::vector<std::int32_t>>> nested_cube(std::int32_t n);
// 通过：3 层嵌套合法

BRIDGE_ASYNC
async_simple::coro::Lazy<std::optional<std::vector<std::optional<std::string>>>>
    complex_nested();
// 通过：optional + vector + optional 嵌套合法

BRIDGE_SYNC
std::unordered_map<std::string, std::vector<std::pair<std::int32_t, std::string>>>
    grouped_data();
// 通过：map + vector + pair 嵌套合法
```

**端到端验证**：需要在 codegen_demo 中实际生成并运行，验证 Dart 侧类型正确（`List<List<List<int>>>`）。

#### G05: 非白名单容器

**反例**：
```cpp
BRIDGE_SYNC
std::shared_ptr<std::int32_t> shared_val();
// 期望报错: unsupported type `std::shared_ptr<int>`

BRIDGE_SYNC
std::map<std::string, std::int32_t> ordered_map();
// 期望报错: unsupported type `std::map<...>`（只支持 unordered_map）

BRIDGE_SYNC
std::variant<std::int32_t, std::string> variant_val();
// 期望报错: unsupported type `std::variant<...>`
```

---

### 4.2 运行时错误传播测试

在 codegen_demo 的 API 头文件中新增故意抛异常的函数，验证 Dart 侧行为。

#### R01: async 函数 throw std::runtime_error

**C++ 实现**：
```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> fail_async(std::string msg) {
    throw std::runtime_error(msg);
    co_return 0;  // unreachable
}
```

**Dart 测试**：
```dart
test('async throw surfaces as StateError', () async {
  await expectLater(
    api.failAsync(msg: 'boom-async'),
    throwsA(isA<StateError>().having(
      (e) => e.message, 'message', contains('boom-async'))),
  );
});
```

#### R02: sync 函数 throw

**C++ 实现**：
```cpp
BRIDGE_SYNC
std::int32_t fail_sync(std::string msg) {
    throw std::runtime_error(msg);
}
```

**Dart 测试**：
```dart
test('sync throw surfaces as StateError', () {
  expect(
    () => api.failSync(msg: 'boom-sync'),
    throwsA(isA<StateError>().having(
      (e) => e.message, 'message', contains('boom-sync'))),
  );
});
```

#### R03: normal 函数 throw

**C++ 实现**：
```cpp
BRIDGE_NORMAL
std::int32_t fail_normal(std::string msg) {
    throw std::runtime_error(msg);
}
```

**Dart 测试**：同 R01 模式。

#### R04: 非 std::exception 异常

**C++ 实现**：
```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<void> fail_non_std() {
    throw 42;  // 非 std::exception
}
```

**Dart 测试**：
```dart
test('non-std exception surfaces as "unknown"', () async {
  await expectLater(
    api.failNonStd(),
    throwsA(isA<StateError>().having(
      (e) => e.message, 'message', contains('unknown'))),
  );
});
```

#### R05: stream 中 throw

**C++ 实现**：
```cpp
BRIDGE_NORMAL
void fail_stream(dcb::StreamSink<std::int32_t> sink, std::string msg) {
    sink.add(1);
    sink.add(2);
    throw std::runtime_error(msg);
}
```

**Dart 测试**：
```dart
test('stream throw delivers partial data then error', () async {
  final values = <int>[];
  Object? err;
  try {
    await for (final v in api.failStream(msg: 'boom-stream')) {
      values.add(v);
    }
  } catch (e) {
    err = e;
  }
  expect(values, [1, 2]);
  expect(err, isA<StateError>());
  expect((err! as StateError).message, contains('boom-stream'));
});
```

#### R06: 异常后 session 仍可正常使用

**Dart 测试**：
```dart
test('session recovers after exception', () async {
  // 先触发异常
  await expectLater(api.failAsync(msg: 'temp'), throwsA(isA<StateError>()));
  // 再正常调用
  final result = await api.add(a: 1, b: 2);
  expect(result, 3);
});
```

---

### 4.3 Opaque 类方法中的异常

#### R07: Opaque 实例方法 throw

**C++ 实现**（在 Counter 类中）：
```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> failIfNegative(std::int32_t v) {
    if (v < 0) throw std::runtime_error("negative value");
    co_return v;
}
```

**Dart 测试**：
```dart
test('opaque method throw surfaces error', () async {
  final counter = Counter(initialValue: 0);
  await expectLater(
    counter.failIfNegative(-1),
    throwsA(isA<StateError>().having(
      (e) => e.message, 'message', contains('negative value'))),
  );
  // 对象仍可正常使用
  counter.increment(5);
  expect(counter.valueSync(), 5);
  counter.dispose();
});
```

---

## 5. 实现优先级

| 批次 | 内容 | 涉及文件 |
|------|------|----------|
| 批次 1 | C01（虚函数）、C02（继承）、C03（模板类）检测 | `codegen/scripts/parse_api.py` `_collect_classes` |
| 批次 2 | F02 增强（未导出类的精确 Hint） | `codegen/scripts/parse_api.py` `_validate_ir` |
| 批次 3 | 运行时异常测试函数 + Dart 测试 | `examples/codegen_demo/native/api/bridge_api.h`、`native/api_impl/bridge_api.cpp`、`test/api_test.dart` |
| 批次 4 | 深层嵌套容器端到端测试 | 同批次 3 |
| 批次 5 | 静态校验反例测试（可选：作为 codegen 的 negative test） | 新增 `codegen/tests/` 或脚本内 assert |

---

## 6. 与现有文档的关系

| 文档 | 关系 |
|------|------|
| `docs/codegen_type_mapping.md` | 定义白名单**是什么**；本文档定义违反白名单时**怎么报错** |
| `docs/known_issues.md` | 记录已接受的技术债；本文档的校验规则是"不允许通过"的硬约束 |
| `codegen/README.md` | 工具链使用说明；本文档补充错误输出格式说明 |

---

## 相关文档

- [codegen_type_mapping.md](./codegen_type_mapping.md) — 类型白名单完整定义
- [frb_and_cpp_bridge_design.md](./frb_and_cpp_bridge_design.md) — 整体架构设计
- [progress.md](./progress.md) — 实现进度
