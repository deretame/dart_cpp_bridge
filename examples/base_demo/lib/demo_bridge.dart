/// Hand-written demo wire dispatch bindings for base_demo.
///
/// This file contains the demo-specific [MethodId] enum, an extension on
/// [DartCppBridge] with typed demo methods, and demo model classes ([Counter],
/// [Person], [StatusCode]). These are NOT part of the base library.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

/// Demo / hand-written method ids (Phase 1; codegen will replace later).
enum MethodId {
  bridgeVersion(1),
  add(2),
  sleepTest(3),
  ticks(4),
  echo(5),
  failAsync(6),
  failStream(7),
  callDartHello(8),
  callDartHelloSync(9),
  maybeDouble(10),
  sumVec(11),
  reverseBytes(12),
  nextStatus(13),
  sumFixedFour(14),
  greet(15),
  scoreTotal(16),
  setSum(17),
  nextI128(18),
  totalAges(19),
  counterCreate(20),
  counterIncrement(21),
  counterGetValue(22),
  counterDrop(23),
  counterValueSync(24),
  counterStaticSum(25),
  counterCallDartFn(26),
  counterSleepAndGet(27),
  counterIncrementStream(28),
  counterCreateDefault(29),
  counterZero(30),
  counterAddList(31),
  counterSetValue(32),
  counterDuplicate(33),
  pairEcho(34),
  tupleEcho(35);

  final int value;
  const MethodId(this.value);
}

/// Demo methods on [DartCppBridge] using the public low-level invoke API.
extension DemoBridge on DartCppBridge {
  /// Sync demo: returns the native bridge protocol version (`i32`).
  int bridgeVersion() {
    return ByteReader(invokeSyncMethod(MethodId.bridgeVersion.value)).i32();
  }

  /// Async demo: `a + b` computed on the C++ side.
  Future<int> add(int a, int b) async {
    final payload = ByteWriter()
      ..i32(a)
      ..i32(b);
    return ByteReader(
            await invokeAsyncMethod(MethodId.add.value, payload.takeBytes()))
        .i32();
  }

  /// Normal-channel demo: sleeps on a worker pool, then returns a done string.
  Future<String> sleepTest() async {
    return ByteReader(await invokeAsyncMethod(MethodId.sleepTest.value)).str();
  }

  /// Stream demo: emits `0 .. count-1` with optional delay between items.
  Stream<int> ticks({int count = 5, int intervalMs = 100}) {
    final payload = ByteWriter()
      ..i32(count)
      ..i32(intervalMs);
    return openStream<int>(
        MethodId.ticks.value, payload.takeBytes(), (r) => r.i32());
  }

  /// Async demo: echoes [s] back from C++.
  Future<String> echo(String s) async {
    final payload = ByteWriter()..str(s);
    return ByteReader(
            await invokeAsyncMethod(MethodId.echo.value, payload.takeBytes()))
        .str();
  }

  /// Async demo: doubles [input] if non-null, returns null otherwise.
  Future<int?> maybeDouble(int? input) async {
    final payload = ByteWriter();
    if (input == null) {
      payload.u8(0);
    } else {
      payload.u8(1);
      payload.i32(input);
    }
    final r = ByteReader(await invokeAsyncMethod(
        MethodId.maybeDouble.value, payload.takeBytes()));
    return r.u8() != 0 ? r.i32() : null;
  }

  /// Async demo: sums a list of ints on the C++ side.
  Future<int> sumList(List<int> values) async {
    final payload = ByteWriter()..u32(values.length);
    for (final v in values) {
      payload.i32(v);
    }
    return ByteReader(await invokeAsyncMethod(
            MethodId.sumVec.value, payload.takeBytes()))
        .i32();
  }

  /// Async demo: reverses a Uint8List on the C++ side.
  Future<Uint8List> reverseBytes(Uint8List input) async {
    final payload = ByteWriter()
      ..u32(input.length)
      ..bytes(input);
    final r = ByteReader(await invokeAsyncMethod(
        MethodId.reverseBytes.value, payload.takeBytes()));
    final n = r.u32();
    return r.takeRest().sublist(0, n);
  }

  /// Async demo: returns the next status code on the C++ side.
  Future<StatusCode> nextStatus(StatusCode current) async {
    final payload = ByteWriter()..i32(current.index);
    return StatusCode.values[ByteReader(await invokeAsyncMethod(
            MethodId.nextStatus.value, payload.takeBytes()))
        .i32()];
  }

  /// Async demo: sums a fixed-length array of 4 ints on the C++ side.
  Future<int> sumFixedFour(List<int> values) async {
    final payload = ByteWriter();
    for (final v in values) {
      payload.i32(v);
    }
    return ByteReader(await invokeAsyncMethod(
            MethodId.sumFixedFour.value, payload.takeBytes()))
        .i32();
  }

  /// Async demo: greets a person from a Dart class on the C++ side.
  Future<String> greet(Person person) async {
    final payload = ByteWriter()
      ..str(person.name)
      ..i32(person.age);
    return ByteReader(await invokeAsyncMethod(
            MethodId.greet.value, payload.takeBytes()))
        .str();
  }

  /// Async demo: sums scores from a Map on the C++ side.
  Future<int> scoreTotal(Map<String, int> scores) async {
    final payload = ByteWriter()..u32(scores.length);
    scores.forEach((k, v) {
      payload
        ..str(k)
        ..i32(v);
    });
    return ByteReader(await invokeAsyncMethod(
            MethodId.scoreTotal.value, payload.takeBytes()))
        .i32();
  }

  /// Async demo: sums values from a Set on the C++ side.
  Future<int> setSum(Set<int> values) async {
    final payload = ByteWriter()..u32(values.length);
    for (final v in values) {
      payload.i32(v);
    }
    return ByteReader(
            await invokeAsyncMethod(MethodId.setSum.value, payload.takeBytes()))
        .i32();
  }

  /// Async demo: echoes a (int, String) pair.
  Future<(int, String)> pairEcho((int, String) input) async {
    final payload = ByteWriter()
      ..i32(input.$1)
      ..str(input.$2);
    final r = ByteReader(await invokeAsyncMethod(
        MethodId.pairEcho.value, payload.takeBytes()));
    return (r.i32(), r.str());
  }

  /// Async demo: echoes a (int, String, bool) tuple.
  Future<(int, String, bool)> tupleEcho((int, String, bool) input) async {
    final payload = ByteWriter()
      ..i32(input.$1)
      ..str(input.$2)
      ..u8(input.$3 ? 1 : 0);
    final r = ByteReader(await invokeAsyncMethod(
        MethodId.tupleEcho.value, payload.takeBytes()));
    return (r.i32(), r.str(), r.u8() != 0);
  }

  /// Async demo: echoes a signed 128-bit integer back as BigInt.
  Future<BigInt> echoI128(BigInt value) async {
    final payload = ByteWriter()..writeI128(value);
    return ByteReader(await invokeAsyncMethod(
            MethodId.nextI128.value, payload.takeBytes()))
        .readI128();
  }

  /// Async demo: sums the ages of a list of people on the C++ side.
  Future<int> totalAges(List<Person> people) async {
    final payload = ByteWriter()..u32(people.length);
    for (final p in people) {
      payload
        ..str(p.name)
        ..i32(p.age);
    }
    return ByteReader(await invokeAsyncMethod(
            MethodId.totalAges.value, payload.takeBytes()))
        .i32();
  }

  /// Async demo: create an opaque Counter object on the C++ side.
  Future<Counter> createCounter({int initialValue = 0}) async {
    final payload = ByteWriter()..i32(initialValue);
    final handle = ByteReader(await invokeAsyncMethod(
            MethodId.counterCreate.value, payload.takeBytes()))
        .u64();
    return Counter.create_(bridge: this, handle: handle);
  }

  /// Default constructor: create a Counter with initial value 0.
  Future<Counter> createCounterDefault() async {
    final handle = ByteReader(
            await invokeAsyncMethod(MethodId.counterCreateDefault.value))
        .u64();
    return Counter.create_(bridge: this, handle: handle);
  }

  /// Factory constructor as a static method: create a Counter with value 0.
  Future<Counter> createCounterZero() async {
    final handle =
        ByteReader(await invokeAsyncMethod(MethodId.counterZero.value)).u64();
    return Counter.create_(bridge: this, handle: handle);
  }

  /// Test helper: C++ always fails this async call with [message].
  Future<void> failAsync([String message = 'fail_async']) async {
    final payload = ByteWriter()..str(message);
    await invokeAsyncMethod(MethodId.failAsync.value, payload.takeBytes());
  }

  /// Test helper: stream emits one value then errors with [message].
  Stream<int> failStream([String message = 'fail_stream']) {
    final payload = ByteWriter()..str(message);
    return openStream<int>(
        MethodId.failStream.value, payload.takeBytes(), (r) => r.i32());
  }

  /// Test helper: invoke an async-only method via the sync FFI entry.
  void invokeSyncNonSyncMethodForTest() {
    invokeSyncMethod(MethodId.add.value);
  }

  /// Test helper: call an unknown method id and expect an error Future.
  Future<void> invokeUnknownMethodForTest() async {
    await invokeAsyncMethod(0x7ffffffe);
  }

  /// Test helper: send a truncated frame and expect an error Future.
  Future<void> invokeBadFrameForTest() async {
    await invokeRawAsync(Uint8List.fromList([1, 2, 3, 4, 5]), responseId: 0);
  }

  /// FRB-style reverse call (C++ **async** wait on io via co_await).
  Future<String> callDartHello(
      FutureOr<String> Function(String name) dartCallback) {
    return invokeStringToStringDartFn(
        MethodId.callDartHello.value, null, dartCallback);
  }

  /// FRB-style reverse call (C++ **sync** block on current native thread).
  Future<String> callDartHelloSync(
      FutureOr<String> Function(String name) dartCallback) {
    return invokeStringToStringDartFn(
        MethodId.callDartHelloSync.value, null, dartCallback);
  }

  /// Helper for string-to-string DartFn reverse calls.
  ///
  /// If [handle] is non-null it is written before the fn_id (for opaque object
  /// methods like `Counter.callCallback`).
  Future<String> invokeStringToStringDartFn(
    int methodId,
    int? handle,
    FutureOr<String> Function(String) dartCallback,
  ) async {
    final fnId = registerDartFn((final argBytes) async {
      final r = ByteReader(argBytes);
      final arg = r.str();
      final res = await dartCallback(arg);
      final w = ByteWriter()..str(res);
      return w.takeBytes();
    });
    final payload = ByteWriter();
    if (handle != null) payload.u64(handle);
    payload.u64(fnId);
    try {
      final result =
          await invokeAsyncMethod(methodId, payload.takeBytes());
      return ByteReader(result).str();
    } finally {
      unregisterDartFn(fnId);
    }
  }
}

/// Demo opaque object for hand-written class-method export test (Counter).
final class Counter extends CppOpaqueInterface {
  Counter._({required super.bridge, required super.handle});

  /// Internal factory used by the [DemoBridge] extension.
  // ignore: library_private_types_in_public_api
  static Counter create_(
      {required DartCppBridge bridge, required int handle}) {
    return Counter._(bridge: bridge, handle: handle);
  }

  /// Create a Counter with [initialValue] via the bridge constructor.
  static Future<Counter> create({int initialValue = 0}) {
    return DartCppBridge.instance.createCounter(initialValue: initialValue);
  }

  /// Default constructor: create a Counter with initial value 0.
  static Future<Counter> defaultCtor() {
    return DartCppBridge.instance.createCounterDefault();
  }

  /// Factory constructor (static method style): create a Counter with value 0.
  static Future<Counter> zero() {
    return DartCppBridge.instance.createCounterZero();
  }

  /// Increment the counter by [delta]. Defaults to 1.
  Future<void> increment([int delta = 1]) async {
    ensureAlive();
    final payload = ByteWriter()
      ..u64(handle)
      ..i32(delta);
    await bridge.invokeAsyncMethod(
        MethodId.counterIncrement.value, payload.takeBytes());
  }

  /// Add all values in [values] to the counter and return the new value.
  Future<int> addList(List<int> values) async {
    ensureAlive();
    final payload = ByteWriter()
      ..u64(handle)
      ..u32(values.length);
    for (final v in values) {
      payload.i32(v);
    }
    return ByteReader(await bridge.invokeAsyncMethod(
            MethodId.counterAddList.value, payload.takeBytes()))
        .i32();
  }

  /// Set the counter to [value] if non-null. Returns the current value.
  Future<int> setValue(int? value) async {
    ensureAlive();
    final payload = ByteWriter()..u64(handle);
    if (value == null) {
      payload.u8(0);
    } else {
      payload.u8(1);
      payload.i32(value);
    }
    return ByteReader(await bridge.invokeAsyncMethod(
            MethodId.counterSetValue.value, payload.takeBytes()))
        .i32();
  }

  /// Create a new Counter with the same current value.
  Future<Counter> duplicate() async {
    ensureAlive();
    final payload = ByteWriter()..u64(handle);
    final newHandle = ByteReader(await bridge.invokeAsyncMethod(
            MethodId.counterDuplicate.value, payload.takeBytes()))
        .u64();
    return Counter._(bridge: bridge, handle: newHandle);
  }

  /// Return the current value (async).
  Future<int> value() async {
    ensureAlive();
    final payload = ByteWriter()..u64(handle);
    return ByteReader(await bridge.invokeAsyncMethod(
            MethodId.counterGetValue.value, payload.takeBytes()))
        .i32();
  }

  /// Return the current value synchronously via the sync FFI entry.
  int valueSync() {
    ensureAlive();
    final payload = ByteWriter()..u64(handle);
    return ByteReader(bridge.invokeSyncMethod(
            MethodId.counterValueSync.value, payload.takeBytes()))
        .i32();
  }

  /// Static method: sum two integers on the C++ side.
  static int sum(int a, int b) {
    final payload = ByteWriter()
      ..i32(a)
      ..i32(b);
    return ByteReader(DartCppBridge.instance.invokeSyncMethod(
            MethodId.counterStaticSum.value, payload.takeBytes()))
        .i32();
  }

  /// Call a Dart callback with the current value (as a string) and return the
  /// result from Dart.
  Future<String> callCallback(
      FutureOr<String> Function(String value) callback) {
    ensureAlive();
    return bridge.invokeStringToStringDartFn(
        MethodId.counterCallDartFn.value, handle, callback);
  }

  /// Normal member method: sleep on the C++ thread pool, then return value.
  Future<int> sleepAndGet(int sleepMs) async {
    ensureAlive();
    final payload = ByteWriter()
      ..u64(handle)
      ..i32(sleepMs);
    return ByteReader(await bridge.invokeAsyncMethod(
            MethodId.counterSleepAndGet.value, payload.takeBytes()))
        .i32();
  }

  /// Stream member method: increment [count] times with [intervalMs] delay.
  Stream<int> incrementStream({int count = 5, int intervalMs = 20}) {
    ensureAlive();
    final payload = ByteWriter()
      ..u64(handle)
      ..i32(count)
      ..i32(intervalMs);
    return bridge.openStream<int>(
      MethodId.counterIncrementStream.value,
      payload.takeBytes(),
      (r) => r.i32(),
    );
  }
}

/// Demo struct for hand-written codegen test (Person).
class Person {
  final String name;
  final int age;

  const Person({required this.name, required this.age});

  @override
  int get hashCode => name.hashCode ^ age.hashCode;

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is Person &&
          runtimeType == other.runtimeType &&
          name == other.name &&
          age == other.age;
}

/// Demo enum for hand-written codegen test (StatusCode).
///
/// Values must match the C++ `StatusCode` enum class ordering.
enum StatusCode {
  ok,
  notFound,
  serverError,
}
