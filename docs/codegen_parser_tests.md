# Codegen Parser 防御性测试规格

> 本文档定义 `codegen/scripts/` 下 Python parser 和 generator 的防御性测试用例。
> 目标：确保 parser 在面对异常输入时给出明确错误信息，而不是生成垃圾代码或静默崩溃。

## 测试基础设施

- 测试文件位置：`codegen/tests/`
- 运行方式：使用 pinned Python 直接执行（不依赖 pytest，用简单 assert + 子进程）
- 每个测试用例构造临时 `.h` 文件和 `dart_cpp_bridge.yaml`，调用 `parse_api.py` / `generate.py`
- 断言：退出码非零 + stderr 包含预期关键字

---

## P 系列：Parser 失败场景

### P01: 语法错误头文件

**输入：**
```cpp
BRIDGE_SYNC
std::int32_t broken_func(std::int32_t a  // 缺少右括号和分号
```

**预期行为：**
- libclang 会报诊断错误（diagnostics）
- parser 应检测到 diagnostics 并输出警告或错误
- 不应生成不完整的 IR 条目

**验证点：**
- 退出码非零，或 IR 中不包含 `broken_func`
- stderr 包含文件名和行号提示

---

### P02: 空/无导出头文件

**输入：**
```cpp
#pragma once
#include <cstdint>

// 没有任何 BRIDGE_* 标记的普通函数
std::int32_t internal_helper(std::int32_t x) { return x * 2; }
```

**预期行为：**
- parser 正常完成，IR 中 `functions` 为空列表
- codegen 生成空的 dispatch（只有 default: unknown method）
- 不报错，但可以有 info 级别提示 "no exported functions found"

**验证点：**
- 退出码 0
- IR `functions` 长度 == 0
- 生成的 wire_dispatch.cpp 可编译（只有 default case）

---

### P03: 重复函数名（重载）

**设计约束：** 不支持 C++ 函数重载，这与 Dart 不支持函数重载保持一致。

**输入：**
```cpp
BRIDGE_SYNC
std::int32_t compute(std::int32_t a);

BRIDGE_SYNC
std::int32_t compute(std::int32_t a, std::int32_t b);
```

**预期行为：**
- method_id 基于函数名哈希，重载会冲突
- 应报明确错误：`duplicate function name 'compute'`
- 错误信息可提示：Dart 不支持函数重载，请使用不同函数名

**验证点：**
- 退出码非零
- stderr 包含 "duplicate" 或 "overload" 关键字
- 指出两个声明的位置

---

### P04: 前向声明 / 不完整类型

**输入：**
```cpp
struct ForwardOnly;  // 只有前向声明

BRIDGE_ASYNC
async_simple::coro::Lazy<ForwardOnly> get_thing();
```

**预期行为：**
- `_validate_ir` 应拦截：`ForwardOnly` 不在白名单、不是已导出的 data_class/opaque_class
- 报错：unsupported type `ForwardOnly`

**验证点：**
- 退出码非零
- stderr 包含 `ForwardOnly` 和 "not supported" / "not exported"

---

### P05: 宏标记用在非函数位置

**输入：**
```cpp
BRIDGE_SYNC
std::int32_t global_counter = 0;  // 变量，不是函数

BRIDGE_EXPORT
namespace SomeNS {}  // 命名空间，不是类
```

**预期行为：**
- parser 应忽略非函数/非类上的标记（或报 warning）
- 不应崩溃或生成无效 IR

**验证点：**
- 不崩溃（退出码 0 或有明确 warning）
- IR 中不包含 `global_counter` 作为函数

---

### P06: 函数指针参数

**输入：**
```cpp
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> apply(std::int32_t (*fn)(std::int32_t));
```

**预期行为：**
- 函数指针不在类型白名单中
- `_validate_ir` 报错：unsupported type

**验证点：**
- 退出码非零
- stderr 包含函数名 `apply` 和 "not supported"

---

### P07: typedef/using 别名穿透

**输入：**
```cpp
using IntVec = std::vector<std::int32_t>;
using NestedVec = std::vector<IntVec>;

BRIDGE_ASYNC
async_simple::coro::Lazy<NestedVec> get_nested();
```

**预期行为（两种可能）：**
- **理想**：libclang 展开别名，parser 识别为 `vector<vector<i32>>`，正常生成
- **降级**：如果无法穿透，应报明确的 "unsupported type: NestedVec" 而不是静默生成错误代码

**验证点：**
- 如果成功：IR 中 return type 为 `{kind: vector, inner: {kind: vector, inner: {kind: i32}}}`
- 如果失败：退出码非零 + 明确错误信息

---

### P08: 自引用 / 循环引用 data_class

**设计约束：** 不支持自引用类型（如链表节点），也不支持循环引用。原因是 encode/decode 会无限递归。

**输入 A（自引用）：**
```cpp
BRIDGE_EXPORT
struct TreeNode {
  std::int32_t value;
  TreeNode left;   // 自引用，C++ 本身也编译不过
  TreeNode right;
};
```

**输入 B（间接循环）：**
```cpp
BRIDGE_EXPORT
struct NodeA {
  NodeB other;
};

BRIDGE_EXPORT
struct NodeB {
  NodeA back;  // 循环引用
};
```

**预期行为：**
- 值类型自引用/循环引用在 C++ 本身就编译不过（incomplete type）
- parser 应检测并报错，而不是让 libclang 崩溃或生成垃圾 IR
- 错误信息指出循环链

**验证点：**
- 退出码非零
- stderr 包含 "circular" / "self-reference" 或指出循环链 `NodeA -> NodeB -> NodeA`

---

### P09: 超长容器嵌套

**输入：**
```cpp
BRIDGE_SYNC
std::vector<std::vector<std::vector<std::vector<std::vector<
  std::vector<std::vector<std::vector<std::vector<std::vector<
    std::int32_t
  >>>>>>>>>> deep_nested();
```

**预期行为（两种策略）：**
- **允许**：递归生成，只要编译器能处理
- **限制**：设置最大嵌套深度（如 8 层），超出报错 "container nesting too deep"

**验证点：**
- 如果允许：生成的代码可编译
- 如果限制：退出码非零 + 明确 "nesting depth" 错误

---

### P10: 非白名单类型一律不支持

**设计约束：** 只有白名单内的类型才支持直接转换，包括：
- 基础类型：`i8/i16/i32/i64`, `u8/u16/u32/u64`, `f32/f64`, `bool`, `string`, `i128/u128`
- 容器：`vector`, `array`, `optional`, `map`, `set`, `pair`, `tuple`（泛型参数也必须是白名单类型）
- 已导出的自定义类型：`enum`, `data_class`, `opaque_class`（需 `BRIDGE_EXPORT` 标记）
- `DartFn` 反向调用类型

**其他所有类型均不支持**，包括但不限于：
- 智能指针：`shared_ptr`, `unique_ptr`, `weak_ptr`
- 原生指针：`T*`, `const T*`
- 其他 STL 类型：`std::filesystem::path`, `std::chrono::*`, `std::variant`, `std::any`
- 未标记 `BRIDGE_EXPORT` 的自定义类

**输入示例：**
```cpp
// 智能指针
BRIDGE_ASYNC
async_simple::coro::Lazy<std::shared_ptr<Counter>> get_counter();

// 原生指针
BRIDGE_SYNC
std::int32_t* get_raw_ptr();

// 未导出的自定义类
struct InternalType { int x; };  // 没有 BRIDGE_EXPORT
BRIDGE_SYNC
InternalType get_internal();
```

**预期行为：**
- `_validate_ir` 拦截所有非白名单类型
- 报错：unsupported type `XXX`
- 对于未导出的类，提示：添加 `BRIDGE_EXPORT` 标记

**验证点：**
- 退出码非零
- stderr 包含具体类型名和 "not supported" / "not in whitelist"

---

### P11: 类型必须定义在指定头文件中

**设计约束：** 只有 yaml `headers` 列表中定义（而非仅引用）的类和结构体才会被导出。
外部头文件中定义的类型即使在函数签名中出现，也不会被自动生成。

**输入：**
```cpp
// external.h（不在 yaml headers 列表中）
struct ExternalType { int x; };

// api.h（在 yaml headers 列表中）
#include "external.h"

BRIDGE_ASYNC
async_simple::coro::Lazy<ExternalType> get_external();
```

**预期行为：**
- `ExternalType` 不是在本项目的导出头文件中定义的
- `_validate_ir` 报错：`ExternalType` 未导出
- 提示：将该类型的定义移到导出头文件中，或将其加入 headers 列表

**验证点：**
- 退出码非零
- stderr 包含 `ExternalType` 和 "not exported" / "not defined in exported headers"

---

## Y 系列：YAML 配置异常

### Y01: headers 路径不存在

**输入 yaml：**
```yaml
headers:
  - non_existent_file.h
```

**预期行为：**
- 立即报错：file not found
- 指出具体哪个路径不存在

**验证点：**
- 退出码非零
- stderr 包含路径和 "not found" / "does not exist"

---

### Y02: yaml 缺少必需字段

**输入 yaml：**
```yaml
# 缺少 headers 字段
output_dir: ./generated
```

**预期行为：**
- 报错：missing required field 'headers'

**验证点：**
- 退出码非零
- stderr 包含 "headers" 和 "required" / "missing"

---

### Y03: include_paths 缺失导致宏不展开

**输入：**
- 头文件使用了 `BRIDGE_EXPORT` 宏
- yaml 中没有配置 `include_paths` 指向 `dart_cpp_bridge/annotate.h`

**预期行为：**
- 宏不展开 → libclang 看不到 annotate 属性 → 没有函数被识别为导出
- 应报 warning："no exported functions found, check include_paths"

**验证点：**
- IR 中 functions 为空
- 有 warning 提示检查 include_paths

---

### Y04: output_dir 不可写

**输入 yaml：**
```yaml
output_dir: /nonexistent/readonly/path
```

**预期行为：**
- 写文件时报错：cannot create directory / permission denied

**验证点：**
- 退出码非零
- stderr 包含路径信息

---

## S 系列：Codegen 输出稳定性

### S01: 幂等性

**操作：**
1. 对同一输入运行 codegen 两次
2. 对比两次输出的所有文件

**预期行为：**
- 两次输出完全一致（byte-for-byte）
- method_id 不漂移

**验证点：**
- `wire_dispatch.hpp`、`wire_dispatch.cpp`、`api_fn.dart`、`api.dart`、`api.g.dart` 全部一致
- `ir.json` 一致

---

### S02: method_id 稳定性

**操作：**
1. 在头文件末尾新增一个函数
2. 重新运行 codegen

**预期行为：**
- 已有函数的 method_id 不变
- 新函数获得新 id
- 不影响已有 Dart 侧调用

**验证点：**
- 对比前后 `ir.json`，已有函数的 `method_id` 字段不变

---

### S03: 函数顺序无关性

**操作：**
1. 交换头文件中两个函数的声明顺序
2. 重新运行 codegen

**预期行为：**
- method_id 基于函数名哈希，与声明顺序无关
- 生成的 dispatch switch case 顺序可能变，但 id 不变

**验证点：**
- 所有函数的 method_id 不变

---

## 实现优先级建议

| 优先级 | 用例 | 理由 |
|--------|------|------|
| P0（必须） | P03, P04, P06, P10 | 用户最容易犯的错误，当前可能静默生成坏代码 |
| P0（必须） | P11 | 类型必须定义在导出头文件中，核心约束 |
| P0（必须） | Y01, Y02 | 配置错误应该立即明确报错 |
| P1（重要） | P01, P02, P05 | parser 健壮性，不应崩溃 |
| P1（重要） | P08 | 自引用/循环引用检测 |
| P1（重要） | S01, S02 | 生成稳定性，避免重新生成后已有调用断裂 |
| P2（增强） | P07, P09 | 边界场景，可以后续迭代 |
| P2（增强） | Y03, Y04, S03 | 用户体验优化 |

---

## 测试执行方式

```bash
# 使用 pinned Python 运行全部测试
cd codegen
dart run bin/codegen.dart tests/run_tests.py

# 或直接：
$env:DCB_PYTHON = "C:\Users\...\python.exe"
& $env:DCB_PYTHON tests/run_tests.py
```

每个测试用例独立，失败时打印：
```
FAIL P03: duplicate function name
  Expected: exit code != 0
  Got:      exit code 0
  IR contained 2 functions (expected error)
```
