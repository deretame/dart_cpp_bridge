# base_demo — 手写 Wire Dispatch 示例

最基础的示例，展示如何使用 `dart_cpp_bridge` 的**手写** wire dispatch 和 Dart 绑定。不涉及 codegen。

这是 Phase 1 的方式：手动编写 C++ dispatch 函数和 Dart extension 方法。新项目建议使用 [codegen_demo](../codegen_demo/) 方式，可自动生成这些层。

## 演示内容

| 功能 | C++ | Dart |
|------|-----|------|
| 同步调用 | `bridge_version()` | `bridge.bridgeVersion()` |
| 异步（协程） | `add(a, b)` → `Lazy<int32_t>` | `await bridge.add(1, 2)` |
| Normal（线程池） | `sleep_test()` | `await bridge.sleepTest()` |
| Stream | `ticks(sink, count, interval)` | `bridge.ticks(count: 5)` |
| DartFn 反向调用 | `callDartHello`（async/sync） | `bridge.callDartHello(cb)` |
| Opaque 对象 | `Counter` 类 | `Counter.create(initialValue: 10)` |
| 错误传播 | `throw std::runtime_error` | `throwsA(isA<StateError>())` |
| 多 Isolate | per-isolate Session | `Isolate.run(...)` |
| 类型覆盖 | optional, vector, map, set, pair, tuple, i128, enum, struct | 对应 Dart 类型 |

## 架构

```text
Dart (ffi_basic_test.dart)
  │  invokeSyncMethod / invokeAsyncMethod / openStream
  ▼
DartCppBridge（基础库）
  │  FFI 二进制帧
  ▼
demo_api.cpp（手写 dispatch）
  │  switch(MethodId) → 业务逻辑
  ▼
Runtime (asio io_context + thread_pool)
```

## 文件结构

```text
base_demo/
├── demo_api.cpp             # 手写 C++ wire dispatch（35 个方法）
├── smoke_main.cpp           # C++ smoke test（无 Dart VM）
├── lib/demo_bridge.dart     # 手写 Dart extension + 模型类
├── test/ffi_basic_test.dart # 50+ 集成测试
├── example/example.dart     # 最小使用示例
├── CMakeLists.txt           # 构建 DLL + smoke test
└── pubspec.yaml
```

## 构建与测试

```bash
# 1. 先构建基础库（获取 asio / async-simple 依赖）
cmake -S ../../dart/native -B ../../dart/native/build
cmake --build ../../dart/native/build --config Release

# 2. 构建本 demo
cmake -S . -B build
cmake --build build --config Release

# 3. C++ smoke test（无需 Dart VM）
./build/Release/dcb_smoke.exe

# 4. Dart 测试
dart pub get
dart test
```

## 核心概念

### 手写 Dispatch

`demo_api.cpp` 在 DLL 加载时注册 dispatch 函数：

```cpp
#ifdef DCB_REGISTER_DISPATCH
const bool _registered = [] {
  dcb::set_dispatch(&dcb::demo::dispatch_request, &dcb::demo::dispatch_sync);
  return true;
}();
#endif
```

dispatch 函数解析帧、按 `MethodId` 路由、将响应回传给 Dart session。

### 手写 Dart 绑定

`lib/demo_bridge.dart` 通过 extension 为 `DartCppBridge` 添加类型化方法：

```dart
extension DemoBridge on DartCppBridge {
  int bridgeVersion() =>
      ByteReader(invokeSyncMethod(MethodId.bridgeVersion.value)).i32();

  Future<int> add(int a, int b) async {
    final payload = ByteWriter()..i32(a)..i32(b);
    return ByteReader(await invokeAsyncMethod(MethodId.add.value, payload.takeBytes())).i32();
  }
}
```

### Opaque 对象（Counter）

C++ 对象存储在 per-session 注册表中，Dart 侧持有 `uint64` handle：

```dart
final counter = await bridge.createCounter(initialValue: 10);
await counter.increment(5);
print(await counter.value());  // 15
counter.dispose();  // 释放 C++ 对象
```

## 何时使用此模式

- 学习 bridge 内部工作原理
- 在 codegen 支持前快速原型验证
- API 数量少的简单项目

生产环境且 API 较多时，请使用 [codegen_demo](../codegen_demo/)。
