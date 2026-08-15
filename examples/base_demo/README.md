# base_demo — Hand-written Wire Dispatch (v2)

This demo follows the current stdexec-based v2 development line. The Dart
wire-facing API is compatible with the released v1 package.

The most basic demo showing how to use `dart_cpp_bridge` with **hand-written** wire dispatch and Dart bindings. No codegen involved.

This is the Phase 1 approach: you manually write the C++ dispatch function and the Dart extension methods. For new projects, prefer the [codegen_demo](../codegen_demo/) approach which auto-generates these layers.

## What's Demonstrated

| Feature | C++ | Dart |
|---------|-----|------|
| Sync call | `bridge_version()` | `bridge.bridgeVersion()` |
| Async (coroutine) | `add(a, b)` → `stdexec::task<int32_t>` | `await bridge.add(1, 2)` |
| Normal (thread pool) | `sleep_test()` | `await bridge.sleepTest()` |
| Stream | `ticks(sink, count, interval)` | `bridge.ticks(count: 5)` |
| DartFn reverse call | `callDartHello` (async/sync) | `bridge.callDartHello(cb)` |
| Opaque object | `Counter` class | `Counter.create(initialValue: 10)` |
| Error propagation | `throw std::runtime_error` | `throwsA(isA<StateError>())` |
| Multi-isolate | per-isolate Session | `Isolate.run(...)` |
| Type coverage | optional, vector, map, set, pair, tuple, i128, enum, struct | corresponding Dart types |

## Architecture

```text
Dart (ffi_basic_test.dart)
  │  invokeSyncMethod / invokeAsyncMethod / openStream
  ▼
DartCppBridge (base library)
  │  FFI binary frames
  ▼
demo_api.cpp (hand-written dispatch)
  │  switch(MethodId) → business logic
  ▼
Runtime (asio io_context + thread_pool)
```

## File Structure

```text
base_demo/
├── demo_api.cpp             # Hand-written C++ wire dispatch (35 methods)
├── smoke_main.cpp           # C++ smoke test (no Dart VM)
├── lib/demo_bridge.dart     # Hand-written Dart extension + models
├── test/ffi_basic_test.dart # 50+ integration tests
├── example/example.dart     # Minimal usage example
├── CMakeLists.txt           # Builds DLL + smoke test
└── pubspec.yaml
```

## Build & Test

```bash
# 1. Build base library first (fetches asio / stdexec)
cmake -S ../../dart/native -B ../../dart/native/build
cmake --build ../../dart/native/build --config Release

# 2. Build this demo
cmake -S . -B build
cmake --build build --config Release

# 3. C++ smoke test (no Dart VM needed)
./build/Release/dcb_smoke.exe

# 4. Dart tests
dart pub get
dart test
```

## Key Concepts

### Hand-written Dispatch

`demo_api.cpp` registers a dispatch function at DLL load time:

```cpp
#ifdef DCB_REGISTER_DISPATCH
const bool _registered = [] {
  dcb::set_dispatch(&dcb::demo::dispatch_request, &dcb::demo::dispatch_sync);
  return true;
}();
#endif
```

The dispatch function parses frames, routes by `MethodId`, and posts responses back to the Dart session.

### Hand-written Dart Bindings

`lib/demo_bridge.dart` extends `DartCppBridge` with typed methods:

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

### Opaque Object (Counter)

C++ objects are stored in a per-session registry. Dart holds a `uint64` handle:

```dart
final counter = await bridge.createCounter(initialValue: 10);
await counter.increment(5);
print(await counter.value());  // 15
counter.dispose();  // releases C++ object
```

## When to Use This Pattern

- Learning how the bridge works internally
- Prototyping before codegen support
- Simple projects with few APIs

For production with many APIs, use [codegen_demo](../codegen_demo/) instead.
