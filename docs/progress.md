# dart_cpp_bridge 实现进度

> 对照设计文档：[frb_and_cpp_bridge_design.md](./frb_and_cpp_bridge_design.md)  
> **已知问题 / 技术债**：[known_issues.md](./known_issues.md)  
> 更新日期：2026-08-02（发布 1.2.4：不再编译 async_simple uthread（EXCLUDE_FROM_ALL），修复 macOS 跨架构 slice 汇编选错；1.2.3 已发布：macOS 按目标架构构建；1.2.2 已发布：修复 `file://` rootUri 路径解析；1.2.1 已于 2026-08-01 发布）

---

## 1. 总览

| 阶段 | 状态 | 说明 |
|------|------|------|
| **Phase 1** 手写骨架 | **基本完成** | Runtime / Session / 四通道 / DartFn io 真挂起 / Dart 包 / 测试 |
| **Phase 2** Codegen | **完成** | 工具链 + scan/标记 + SYNC/ASYNC/NORMAL + Dart 三层 + 类型校验 + 模板；见 `examples/codegen_demo` |
| **Phase 3** Native Assets + 生产 | **核心完成** | Native Assets hook 已接线（`hook/build.dart` + `hook/link.dart`）；codegen 仍手动；1.2.4 已发布 |
| **Phase 4** 业务接入 | 未开始 | 不替换任何已有 FRB 生产桥 |

当前仓库已发布 **1.2.4**（`dart_cpp_bridge` 与 `dcb_gen_tool` 版本同步），
与 Breeze 等业务仓解耦。公开 API / C ABI / wire 协议已稳定，变更需遵守
`AGENTS.md` 的兼容性政策。

Dart 测试：`cd dart && dart test`（codec + FFI 原生测试）。  
C++ 冒烟：`build/Release/dcb_smoke.exe`（oneshot 跨线程、io 不堵、DartFn e2e、coro::sleep 可取消、ForeignExecutor 回退取消）。

---

## 2. 已落地（相对设计 §0 锁定决策）

### 2.1 运行时与会话

| 设计项 | 实现 | 备注 |
|--------|------|------|
| asio `io_context` **单线程** | ✅ | `Runtime` |
| `AsioExecutor`（async_simple） | ✅ | `schedule` → `asio::post(io)` |
| `asio::thread_pool` + post | ✅ | normal / ticks 间隔 sleep |
| `spawn_blocking` → Lazy | ⚠️ 部分 | API 仍在；normal 路径多用 pool post |
| `spawn_on_asio` + `via(executor)` | ✅ | factory 保活到 Lazy 结束（防 coroutine lambda capture 悬空） |
| 长期 reply port + `request_id` | ✅ | |
| **Runtime 进程唯一** | ✅ | |
| **Session 每 Isolate 一个** | ✅ | `SessionRegistry` + `dcb_session_open` |
| dispose = generation，晚到 post 丢弃 | ✅ | |
| 不做运行时 CancelToken（协作式信号取消由业务层暴露） | ✅ | Signal/Slot + 可取消 sleep + `collectAny/All<Terminate>` |
| Stream 关订阅后 add 静默丢 | ✅ | `dcb_stream_close` |
| NativeFinalizer 自动关 session | ✅ | 对齐 FRB：日常可不手动 dispose |
| 可选 `dispose` / 进程 `shutdown` | ✅ | worker 勿调 shutdown |
| **DartFn 反向调用（参数式）** | ✅ | 见下表 |
| oneshot channel（`co::oneshot`） | ✅ | `include/dart_cpp_bridge/channel.hpp` |
| channel awaiter 持 shared state | ✅ | `recv()` 的 awaitable/awaiter 持 `shared_ptr<state>`，Receiver 先销毁/移动也不悬垂 |
| mpsc channel 节点化 + stop token 取消 | ✅ | send/recv park 改侵入式节点；tokio 式值撤回；stop token 协作取消；见 channel_stop_token_design.md |
| `StreamSink` 持 `shared_ptr<Session>` | ✅ | sink 可长期持有；session 关闭后 add/end/error 经 generation 检查静默丢弃 |

#### DartFn 仿函数模式

| C++ API | 行为 | 谁承担风险 |
|---------|------|------------|
| `co_await fn(args...)` | **io 上** `co_await` oneshot，**真挂起、不堵 io、不占 pool** | 需 Lazy 绑 Executor（`spawn_on_asio` / `.via()` 已保证） |
| `syncAwait(spawn(fn(args...)))` | **当前线程**阻塞直到 Dart reply | 禁止在 io 线程调用（自死锁）；在 pool / 外部线程使用 |

链路（对齐 FRB oneshot）：

```text
io:  post DartFnCall → co_await rx.recv()（挂起）
Dart Isolate: 回调 → dcb_dart_fn_reply → oneshot.send
io:  Executor::schedule(resume) → 继续
```

Dart 侧回调无论 sync/async 都在 **Isolate 事件循环**执行。细节与历史踩坑见 [known_issues.md](./known_issues.md) §1（**已解决**）。

### 2.2 通道与错误

| 通道 | Demo API | Dart | 测试 |
|------|----------|------|------|
| sync | `bridgeVersion` | `int` | ✅ |
| async（Lazy） | `add` / `echo` / `failAsync` | `Future` | ✅ |
| normal（线程池） | `sleepTest` | `Future` | ✅ |
| stream | `ticks` / `failStream` | `Stream` | ✅ |
| DartFn async（io 挂起） | `callDartHello` | `Future` + 闭包 | ✅ |
| DartFn sync（当前线程堵） | `callDartHelloSync` | `Future` + 闭包 | ✅ |
| wire 双 catch | 全部路径 | 抛 `StateError` | ✅ |
| 坏帧 / 未知 method / sync 误用 | — | — | ✅ |

### 2.3 Dart 包

| 项 | 状态 |
|----|------|
| `dart/` 纯 Dart 包 + FFI | ✅ |
| `DartCppBridge.init` 每 isolate | ✅ |
| Completer / StreamController 多路复用 | ✅ |
| 多 isolate async + stream | ✅ 测试覆盖 |
| Finalizer 自动 session_close | ✅ |

### 2.4 构建与工具

| 项 | 状态 |
|----|------|
| CMake + FetchContent asio / async-simple | ✅ |
| vendored `third_party/dart_api` | ✅（`cmake/fetch_dart_api.cmake`） |
| `dcb_smoke` 原生冒烟 | ✅（含 oneshot / DartFn e2e） |
| `dart test`（codec + FFI） | ✅ **38** 例量级 |
| 远端固定版 Python/libclang codegen | ✅ | lock + cache + scan/标记；已生成 SYNC/ASYNC/NORMAL + enum/optional/容器/Int128/UInt128/DartFn；Dart 三层（impl/单例/顶层函数）；见 `examples/codegen_demo` |
| Dart CLI 工具链 (`dcb_codegen`) | ✅ | 替代旧 PowerShell/Shell 脚本；`dart run bin/codegen.dart` 一键 bootstrap + 运行 |
| 显式类标记 `BRIDGE_DATA_CLASS` / `BRIDGE_OPAQUE` | ✅ | 对齐 FRB `RustAutoOpaque` 模式；opaque 类忽略公开字段，只生成方法 |
| Native Assets hook | ❌ |
| examples 用户模板 + PUBLIC 暴露依赖 | ⏳ | `codegen_demo` 可作模板雏形；未产品化 FetchContent 接入 |

---

## 3. 与设计文档的差异 / 演进

设计原文偏「单 session + 长期 port」。实现中为支持**后台 Isolate 异步**，演进为：

```text
Runtime（进程唯一）
  └─ Session × N（每个调用 init 的 Isolate 一个 reply port）
```

- 业务仍不感知 port；wire 持有 `shared_ptr<Session>`。
- 生命周期：`init` 必调；**dispose 可选**（Finalizer 兜底）；`shutdown` 仅进程退出。

其余锁定决策（无取消、Dart 抛异常、codec 逻辑类型等）保持不变。  
Codegen：宏在 libclang 路径用 `annotate("bridge::*")`（非 `[[bridge.*]]`）；Dart 生成 **impl + 单例 + 顶层函数** 三层。

---

## 4. 目录与入口

```text
docs/
  codegen_type_mapping.md       # 类型映射白名单
  frb_and_cpp_bridge_design.md   # 设计全文
  progress.md                    # 本进度
  known_issues.md                # 技术债与已解问题
include/dart_cpp_bridge/         # 公共头（含 channel / asio_executor）
src/runtime|wire|ffi_entry       # 实现
dart/lib + dart/test             # Dart 包与测试
dcb_gen_tool/                    # CLI 工具 + parse/generate
examples/codegen_demo/           # Phase 2 fixture（yaml + 生成 + dart test）
```

常用命令：

```powershell
# C++ 主库（需在 VS Developer PowerShell 中运行）
cmake -S . -B build
cmake --build build --config Release
.\build\Release\dcb_smoke.exe

# Dart 主包测试（需先编出 dll）
cd dart
dart test

# Codegen demo
cd codegen
dart pub get
dart run bin/codegen.dart scripts/run_codegen.py ../examples/codegen_demo/dart_cpp_bridge.yaml
cd ..\examples\codegen_demo
cmake -S . -B build && cmake --build build --config Release
dart test
```

---

## 5. Phase 2 Codegen 推进计划

按“小步快跑、每步都跑通 `examples/codegen_demo` 端到端测试”的顺序推进：

1. **枚举生成** ✅
   - 解析 `enum class T : int32_t` / `enum T`。
   - C++ wire 按底层 `i32` 编解码。
   - Dart 生成同名 `enum T`，函数签名/返回值使用 `T`。
   - 在 `examples/codegen_demo` 添加 `next_status(OrderStatus)` 测试并跑通。

2. **可选类型 `std::optional<T>` 生成** ✅
   - 支持 `T` 为 `i32` / `u32` / `i64` / `bool` / `std::string` / 已生成 enum。
   - Wire 格式：1-byte presence tag（0 = null，1 = 有值）+ `T` 的编码。
   - C++ 生成使用现有 `ByteReader::opt` / `ByteWriter::opt` 模板。
   - Dart 生成 `T?`，读写生成内联 `u8` tag + value。
   - 在 `examples/codegen_demo` 添加 `maybe_double(std::optional<int32_t>)` 测试：null → null，有值 → 值 × 2。

3. **补齐基础类型白名单测试** ✅
   - 目标：在 `examples/codegen_demo` 把 codegen 已支持的所有原子类型都跑通端到端。
   - 覆盖 `int32_t` / `uint32_t` / `int64_t` / `bool` / `std::string` / `enum` / `std::optional<T>`（`T` 为前述类型）。
   - 每个类型至少一个 BRIDGE_ASYNC 往返函数；记录 Dart `int` ↔ wire 有符号/无符号语义。
   - 先不进入 struct / 容器 / tuple / Stream / DartFn / 类方法。

4. **容器与 128 位整数生成** ✅
   - 容器：`std::vector<T>` / `std::array<T, N>` → `List<T>`，`std::unordered_map<K, V>` → `Map<K, V>`，`std::unordered_set<T>` → `Set<T>`。
   - 元素类型递归支持白名单内基础类型；`std::array` 长度从模板参数解析。
   - 128 位整数：仅支持本项目的 `dcb::Int128` / `dcb::UInt128`，wire 上用固定标记位 + 长度前缀十进制字符串；Dart 侧为 `BigInt`。
   - 在 `examples/codegen_demo` 为每种容器和 128 添加往返测试。
   - 修复了 libclang 在 Windows 上无法解析 `std::vector` / `std::unordered_map` / `std::unordered_set` 的问题：codegen 自动把 `build/_deps/async_simple-src` 与 `build/_deps/asio-src/asio/include` 加入 include 路径，使模板实例化信息完整。

5. **DartFn 生成** ✅
   - 支持泛型签名 `dcb::DartFn<Ret(Args...)>`（语法类似 `std::function`），例如 `dcb::DartFn<std::string(std::string)>`。
   - Dart 侧按实际参数/返回值类型生成 `Future<Ret> Function(Args...)`，注册/注销二进制回调；C++ 侧生成带 encode/decode lambda 的 `dcb::DartFn<Signature>` 后反向调用。
   - 修复生成代码中 DartFn `fn_id` 写入顺序与 C++ 读取顺序不一致的问题：现在 `fn_id` 严格按参数顺序写入 payload。
   - 修复 MSVC 上模板参数包默认实参限制：便利构造函数改用 `requires` + `std::tuple` 比较。
   - 在 `examples/codegen_demo` 添加 `greet_dart_fn` 测试（含 sync / async 闭包）并跑通。

6. **元组生成** ✅
   - 目标：`std::pair<T1, T2>` / `std::tuple<Ts...>` → Dart Record `(T1, T2)` / `(T1, T2, ...)`。
   - wire 按位置顺序依次编解码，不传输长度或字段名（元素个数编译期确定）。
   - 实现点：
     - IR：新增 `"kind": "pair"` / `"tuple"`，元素类型递归进入白名单。
     - C++ 生成：参数/返回值使用现有 `ByteWriter::pair/tuple` 和 `ByteReader::pair/tuple` 模板辅助。
     - Dart 生成：inline 按位置逐个 write/read，类型用 Dart Record 表示。
   - 测试：`examples/codegen_demo` 添加 `pair_echo(std::pair<int, std::string>)` 和 `tuple_echo(std::tuple<int, std::string, bool>)` 两个 BRIDGE_ASYNC 往返函数并跑通。

7. **Stream 生成** ✅
   - 目标：C++ `StreamSink<T>` 参数映射为 Dart `Stream<T>`；函数返回 `void`，由业务代码在内部通过 sink 异步/多线程发数据。
   - 实现点：
     - IR：`StreamSink<T>` 参数标记为 `"kind": "stream_sink"`；**带导出标记（通常 `BRIDGE_NORMAL`）**且含 `StreamSink<T>` 参数的函数归类为 `stream`（只有 sink 参数、无导出标记的函数不生成，解析器告警）。
     - 可选 stream：`std::optional<StreamSink<T>>` 参数用于 `BRIDGE_SYNC` / `BRIDGE_ASYNC` / `BRIDGE_NORMAL` 函数，Dart 侧变为 `StreamController<T>?` 输入参数（sync 事件在 FFI 调用返回后送达）。
     - C++ 生成：从请求 payload 读出其余参数，构造带 encode lambda 的 `dcb::StreamSink<T>`，调用业务函数；stream id 复用 `request_id`。
     - Dart 运行层：把 `_streams` 从 `Map<int, StreamController<int>>` 改为带类型化 decoder 的 `Map<int, _StreamSubscription>`，并新增公共 `openStream<T>(methodId, payload, decodeItem)` 方法。
     - Dart 生成：stream 方法返回 `Stream<T>`，构造 payload 后调用 `bridge.openStream<T>(...)`。
     - `StreamSink<T>` 模板参数从 encode lambda 类型改为 value 类型 `T`，内部用 `std::function` 类型擦除，使业务 API 可以写 `dcb::StreamSink<std::int32_t>`。
     - 手写 `ticks()` / `_counterIncrementStream` / `failStream` 改用新的 `openStream<int>`。
   - 测试：`examples/codegen_demo` 添加 `tick_stream(StreamSink<int>, int count, int interval_ms)` fixture 并跑通（含正常结束和取消订阅）。

8. **结构体/数据类生成** ✅
   - 设计已写入 `docs/codegen_type_mapping.md` §5.2。
   - 实现点：
     - `parse_api.py`：识别 `BRIDGE_EXPORT` 的 `struct` / `class`，收集 public 非静态字段；若类无导出方法则标记为 `"kind": "data_class"`。
     - IR：每个 data_class 记录 `name`、`qualified`、`fields`（含 name + type_ir）。
     - `_type_ir`：遇到已注册 data_class 的 qualified name 时返回 `"kind": "data_class"`。
     - `generate.py`：
       - C++：为每个 data_class 生成 `encode_<Name>` / `decode_<Name>`，在 wire dispatch 中内联使用。
       - Dart：生成不可变 Dart class（字段、`const` 构造函数、`==`、`hashCode`）。
   - Fixture：`examples/codegen_demo/native/api/point_rect.h` 新增 `Point`、`Rect` 和顶层函数 `distance(Point, Point)` / `scale(Point, double)` / `boundingBox(List<Point>)`。
   - 测试：`examples/codegen_demo` 36 例 demo 测试全绿，数据类作为参数、返回值、嵌套以及 `List<data_class>` 均已端到端跑通。

9. **类型白名单校验与友好报错** ✅
   - `_type_ir` 返回 `unsupported` 时携带源文件/行号（`loc` 字段）。
   - `parse_project` 在 IR 组装后执行 `_validate_ir` 验证 pass，递归检查所有函数参数/返回值、类字段/方法中的不支持类型。
   - 报错格式：清晰的 `=` 分隔框 + 上下文（函数名/字段名 + file:line）+ Hint 提示 + 白名单文档引用。
   - 数据类字段出现 Opaque 类时给出明确错误（opaque 类为 handle-only，不能按值嵌入 data class）。
   - 验证不通过时 `SystemExit` 终止 codegen，不会生成错误代码。

10. **用户模板产品化** ✅
    - `examples/codegen_demo/README.md` 重写为可复制的项目模板：Quick Start、配置说明、自定义清单、FetchContent 接入示例。
    - `CMakeLists.txt` 添加完整的模板注释头（接入方式 A/B、自定义清单）和分段注释。
    - CMake FetchContent 接入已文档化（Phase 3 产品化前的过渡说明）。

11. **类方法导出（opaque 对象）生成** ✅
    - 设计已写入 `docs/codegen_type_mapping.md` §5.3。
    - 运行时已提供 `dcb::ObjectHandleRegistry`（per-Session）和 `dcb_drop_object`；codegen 只需调用。
    - 实现点：
      - `parse_api.py`：
        - 识别带 `BRIDGE_OPAQUE` / `BRIDGE_DATA_CLASS` 的 `class` / `struct`。
        - 扫描类内 public 方法，按 `BRIDGE_SYNC/ASYNC/NORMAL` 分类。
        - 识别 `BRIDGE_CONSTRUCTOR` / `BRIDGE_DESTRUCTOR`（约定兜底）。
        - **显式标记**：`BRIDGE_DATA_CLASS` → data_class；`BRIDGE_OPAQUE` → opaque_class（忽略公开字段）。
        - 自动为每个 opaque class 注入 `aliveCount()` 诊断方法到 IR（标记 `"generated": true`）。
      - IR：每个 opaque_class 记录 `name`、`qualified`、`fields`（可选，当前阶段不导出字段）、`methods`。
      - `_type_ir`：opaque 类作为参数/返回值时统一按 `"kind": "opaque_handle"` 处理。
      - `generate.py`：
        - C++：构造函数生成 insert 到 `ObjectHandleRegistry` 并返回 handle；实例方法 payload 首字段为 handle；析构复用 `dcb_drop_object`。
        - C++：每个 opaque class 自动生成 per-session alive 计数器（`AliveCounter_ClassName` 结构体，内含 `mutex` + `unordered_map<session_id, count>`），构造时 `increment`，DropFn 中 `decrement`。
        - C++：`aliveCount()` 静态方法读取当前 session 的存活实例数。
        - Dart：生成 `class Counter extends CppOpaqueInterface`，实例方法首参数隐藏 `_handle`；自动生成 `Counter.aliveCount()` 静态方法。
    - **析构机制（对齐 FRB `RustAutoOpaque` 模式）**：
      - `BRIDGE_DESTRUCTOR` 为 no-op 标记，不生成 wire 方法；析构统一由 `shared_ptr` 生命周期管理。
      - Dart `dispose()` → `dcb_drop_object(handle)` → registry 移除 → `shared_ptr` 引用归零 → C++ `~ClassName()`。
      - 若未手动 dispose，Dart GC 通过 `NativeFinalizer` 自动触发相同清理。
      - 用户只需在 `~ClassName()` 中写自定义清理逻辑（关文件、释放资源等），无需手动管理计数。
    - **显式类标记（对齐 FRB）**：
      - `BRIDGE_DATA_CLASS`：显式标记纯数据类，校验不能有导出方法。
      - `BRIDGE_OPAQUE`：显式标记 opaque 类，公开字段被忽略，只生成标注的方法。若需访问字段，手写 `BRIDGE_SYNC` getter/setter。
    - Fixture：在 `examples/codegen_demo/native/api/counter.h` 新增生成版 `Counter`（使用 `BRIDGE_OPAQUE`），覆盖默认构造、带参构造、sync/async/static/DartFn/Normal/Stream 实例方法、独立句柄、dispose/跨 Isolate 拒绝、`BRIDGE_DESTRUCTOR` 析构标记等场景。`Point`/`Rect` 使用 `BRIDGE_DATA_CLASS`。
    - 测试：`examples/codegen_demo` 47 例 demo 测试全绿（含 Counter 16 例，其中析构相关 3 例）；`dart/` 主包 82 例全绿。
    - 限制：当前阶段不导出 Opaque 类字段、不支持方法重载、不支持多态继承。

---

## 6. 下一步建议

1. 按上述 Phase 2 顺序逐个实现并跑通 `examples/codegen_demo` 端到端测试。
2. **Phase 3**：Native Assets hook；CMake export。
3. 可选：`spawn_blocking` 复用 oneshot；跨平台 CI。
4. 完善 `dcb_gen_tool` 的逻辑，优化操作。
5. 完善文档。

---

## 7. 一句话

**Phase 1 手写桥已跑通；Phase 2 codegen 已能扫标记头并生成 SYNC/ASYNC/NORMAL + enum / optional / 容器 / Int128 / UInt128 / DartFn / tuple / Stream / 数据类 / Opaque 类方法 + BRIDGE_DESTRUCTOR 析构 + per-session alive 计数（C++ wire + Dart 三层）+ 类型白名单校验友好报错 + 用户模板产品化 + 显式类标记（BRIDGE_DATA_CLASS / BRIDGE_OPAQUE，对齐 FRB）+ Dart CLI 工具链，fixture 见 `examples/codegen_demo`；所有当前支持类型均已端到端测试通过。Phase 2 完成。**

