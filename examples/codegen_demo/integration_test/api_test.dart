import 'dart:async';

import 'package:codegen_demo/codegen_demo.dart';
import 'package:codegen_demo/main.dart' as app;
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

/// Polls until [condition] becomes true (used to wait until a cancellable
/// task has registered its signal in the native registry).
Future<void> _waitUntil(
  bool Function() condition, {
  Duration timeout = const Duration(seconds: 5),
}) async {
  final deadline = DateTime.now().add(timeout);
  while (!condition()) {
    if (DateTime.now().isAfter(deadline)) {
      fail('condition not met within $timeout');
    }
    await Future<void>.delayed(const Duration(milliseconds: 10));
  }
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  // integration test 的标准做法：显式重新启动 app，让当前 binding 拥有合法的
  // widget tree 和 FocusManager，避免旧 FocusManager dispose 后继续收到事件。
  app.main();

  setUpAll(() async {
    await DcbLib.init();
  });

  tearDownAll(() {
    DcbLib.shutdown();
  });

  testWidgets('BRIDGE_SYNC bridge_version', (tester) async {
    expect(bridgeVersion(), 42);
  });

  testWidgets('BRIDGE_ASYNC add', (tester) async {
    expect(await add(a: 2, b: 3), 5);
  });

  testWidgets('BRIDGE_NORMAL sleep_greeting', (tester) async {
    expect(await sleepGreeting(name: 'Ada'), 'hello, Ada');
  });

  testWidgets('BRIDGE_ASYNC enum next_status', (tester) async {
    expect(await nextStatus(current: OrderStatus.created), OrderStatus.paid);
    expect(await nextStatus(current: OrderStatus.paid), OrderStatus.shipped);
    expect(await nextStatus(current: OrderStatus.shipped), OrderStatus.created);
  });

  testWidgets('BRIDGE_ASYNC optional maybe_double', (tester) async {
    expect(await maybeDouble(), isNull);
    expect(await maybeDouble(value: 5), 10);
    expect(await maybeDouble(value: -3), -6);
  });

  testWidgets('BRIDGE_ASYNC u32 increment_u32', (tester) async {
    expect(await incrementU32(value: 0), 1);
    expect(await incrementU32(value: 4294967290), 4294967291);
  });

  testWidgets('BRIDGE_ASYNC i64 increment_i64', (tester) async {
    expect(await incrementI64(value: 0), 1);
    expect(await incrementI64(value: 9223372036854775800), 9223372036854775801);
    expect(
      await incrementI64(value: -9223372036854775800),
      -9223372036854775799,
    );
  });

  testWidgets('BRIDGE_ASYNC bool negate_bool', (tester) async {
    expect(await negateBool(value: true), false);
    expect(await negateBool(value: false), true);
  });

  testWidgets('BRIDGE_ASYNC optional string', (tester) async {
    expect(await optionalString(), isNull);
    expect(await optionalString(value: 'hello'), 'hello!');
  });

  testWidgets('BRIDGE_ASYNC optional enum', (tester) async {
    expect(await optionalStatus(), isNull);
    expect(await optionalStatus(value: OrderStatus.created), OrderStatus.paid);
    expect(
      await optionalStatus(value: OrderStatus.shipped),
      OrderStatus.created,
    );
  });

  testWidgets('BRIDGE_ASYNC vector<int> echo_list', (tester) async {
    expect(await echoList(values: []), <int>[]);
    expect(await echoList(values: [1, 2, 3]), [1, 2, 3]);
    expect(await echoList(values: [-1, 0, 42]), [-1, 0, 42]);
  });

  testWidgets('BRIDGE_ASYNC vector<bool> echo_bool_list', (tester) async {
    expect(await echoBoolList(values: []), <bool>[]);
    expect(await echoBoolList(values: [true, false, true]), [true, false, true]);
    expect(await echoBoolList(values: [false]), [false]);
  });

  testWidgets('BRIDGE_ASYNC array<int, 4> sum_array', (tester) async {
    expect(await sumArray(values: [1, 2, 3, 4]), 10);
    expect(await sumArray(values: [-1, 1, -1, 1]), 0);
  });

  testWidgets('BRIDGE_ASYNC map<string, int> sum_scores', (tester) async {
    expect(await sumScores(scores: {}), 0);
    expect(await sumScores(scores: {'a': 1, 'b': 2, 'c': 3}), 6);
  });

  testWidgets('BRIDGE_ASYNC set<int> sum_set', (tester) async {
    expect(await sumSet(values: <int>{}), 0);
    expect(await sumSet(values: {1, 2, 3}), 6);
  });

  testWidgets('BRIDGE_ASYNC ordered map<std::string, int> sum_scores_ordered',
      (tester) async {
    expect(await sumScoresOrdered(scores: {}), 0);
    expect(await sumScoresOrdered(scores: {'a': 1, 'b': 2, 'c': 3}), 6);
  });

  testWidgets('BRIDGE_ASYNC ordered set<int> sum_set_ordered',
      (tester) async {
    expect(await sumSetOrdered(values: <int>{}), 0);
    expect(await sumSetOrdered(values: {1, 2, 3}), 6);
  });

  testWidgets('BRIDGE_ASYNC Int128 echo_i128', (tester) async {
    final big = BigInt.parse('170141183460469231731687303715884105727');
    expect(await echoI128(value: big), big);
    expect(await echoI128(value: BigInt.zero), BigInt.zero);
    expect(
      await echoI128(
        value: BigInt.parse('-170141183460469231731687303715884105728'),
      ),
      BigInt.parse('-170141183460469231731687303715884105728'),
    );
  });

  testWidgets('BRIDGE_ASYNC UInt128 echo_u128', (tester) async {
    final big = BigInt.parse('340282366920938463463374607431768211455');
    expect(await echoU128(value: big), big);
    expect(await echoU128(value: BigInt.zero), BigInt.zero);
  });

  testWidgets('BRIDGE_ASYNC DartFn greet_dart_fn', (tester) async {
    expect(
      await greetDartFn(callback: (name) async => 'Dart $name', name: 'world'),
      'hello, Dart world',
    );
    expect(
      await greetDartFn(
        callback: (name) async {
          await Future<void>.delayed(const Duration(milliseconds: 10));
          return 'async $name';
        },
        name: 'moon',
      ),
      'hello, async moon',
    );
  });

  testWidgets('BRIDGE_NORMAL DartFn syncAwait with two args', (tester) async {
    expect(
      await concatDartFn(callback: (a, b) async => '$a+$b', a: 'foo', b: 'bar'),
      'sync:foo+bar',
    );
    expect(
      await concatDartFn(callback: (a, b) async => '$b-$a', a: 'X', b: 'Y'),
      'sync:Y-X',
    );
  });

  testWidgets('FRB-style: sync register + async invoke (no deadlock)', (
    tester,
  ) async {
    final ok = registerDartFn(callback: (s) async => 'echo:$s');
    expect(ok, isTrue);

    final result = await invokeRegistered(input: 'world');
    expect(result, 'registered:echo:world');

    registerDartFn(callback: (s) async => s.toUpperCase());
    final result2 = await invokeRegistered(input: 'hello');
    expect(result2, 'registered:HELLO');
  });

  testWidgets(
    'FRB-style: sync register + coroutine invoke (co_await fn(...))',
    (tester) async {
      registerDartFn(callback: (s) async => 'co:$s');

      final result = await invokeRegisteredAsync(input: 'coroutine');
      expect(result, 'async_registered:co:coroutine');

      registerDartFn(callback: (s) async => '${s.length}');
      final result2 = await invokeRegisteredAsync(input: 'abcd');
      expect(result2, 'async_registered:4');
    },
  );

  testWidgets('BRIDGE_ASYNC pair<int, string> pair_echo', (tester) async {
    expect(await pairEcho(value: (1, 'hello')), (1, 'hello'));
    expect(await pairEcho(value: (-42, 'world')), (-42, 'world'));
  });

  testWidgets('BRIDGE_ASYNC tuple<int, string, bool> tuple_echo', (
    tester,
  ) async {
    expect(await tupleEcho(value: (1, 'hello', true)), (1, 'hello', true));
    expect(await tupleEcho(value: (-42, 'world', false)), (
      -42,
      'world',
      false,
    ));
  });

  testWidgets('Stream tick_stream emits 0..count-1 then done', (tester) async {
    final values = await tickStream(count: 5, intervalMs: 10).toList();
    expect(values, [0, 1, 2, 3, 4]);
  });

  testWidgets('Stream tick_stream cancels subscription', (tester) async {
    final stream = tickStream(count: 100, intervalMs: 10);
    final sub = stream.listen(null);
    await Future<void>.delayed(const Duration(milliseconds: 30));
    await sub.cancel();
  });

  testWidgets('optional StreamSink downloadWithProgress with progress', (
    tester,
  ) async {
    final progressValues = <int>[];
    final controller = StreamController<int>();
    controller.stream.listen(progressValues.add);

    final result = await downloadWithProgress(
      url: 'https://example.com/file.zip',
      progress: controller,
    );

    expect(result, 'downloaded: https://example.com/file.zip');
    expect(progressValues, [20, 40, 60, 80, 100]);
    await controller.close();
  });

  testWidgets('optional StreamSink downloadWithProgress without progress', (
    tester,
  ) async {
    final result = await downloadWithProgress(url: 'test.txt');
    expect(result, 'downloaded: test.txt');
  });

  testWidgets('sync + optional StreamSink syncDownloadWithProgress with progress', (
    tester,
  ) async {
    final progressValues = <int>[];
    final controller = StreamController<int>();
    controller.stream.listen(progressValues.add);

    final result = syncDownloadWithProgress(
      url: 'https://example.com/file.zip',
      progress: controller,
    );

    expect(result, 'downloaded: https://example.com/file.zip');
    // Events posted by C++ during the blocking FFI call are delivered on the
    // reply port right after the call returns.
    await Future<void>.delayed(const Duration(milliseconds: 50));
    expect(progressValues, [20, 40, 60, 80, 100]);
    await controller.close();
  });

  testWidgets('sync + optional StreamSink syncDownloadWithProgress without progress', (
    tester,
  ) async {
    final result = syncDownloadWithProgress(url: 'test.txt');
    expect(result, 'downloaded: test.txt');
  });

  testWidgets('data class distance', (tester) async {
    final a = const Point(x: 0.0, y: 0.0);
    final b = const Point(x: 3.0, y: 4.0);
    expect(await distance(a: a, b: b), closeTo(5.0, 1e-9));
    expect(await distance(a: a, b: a), closeTo(0.0, 1e-9));
  });

  testWidgets('data class toString (default + custom dart_code)', (
    tester,
  ) async {
    const p = Point(x: 1.0, y: 2.0);
    expect(p.toString(), 'Point(x: 1.0, y: 2.0, label: null)');
    const r = Rect(
      topLeft: Point(x: 0.0, y: 0.0),
      bottomRight: Point(x: 3.0, y: 4.0),
    );
    expect(
      r.toString(),
      'Rect[Point(x: 0.0, y: 0.0, label: null) -> Point(x: 3.0, y: 4.0, label: null)]',
    );
  });

  testWidgets('data class scale', (tester) async {
    final p = const Point(x: 1.5, y: -2.0);
    final scaled = await scale(p: p, factor: 2.0);
    expect(scaled.x, closeTo(3.0, 1e-9));
    expect(scaled.y, closeTo(-4.0, 1e-9));
    expect(scaled.label, isNull);
  });

  testWidgets('data class optional field round-trip', (tester) async {
    final p = const Point(x: 1.0, y: 2.0, label: 'hello');
    final scaled = await scale(p: p, factor: 3.0);
    expect(scaled.x, closeTo(3.0, 1e-9));
    expect(scaled.y, closeTo(6.0, 1e-9));
    expect(scaled.label, 'hello');
  });

  testWidgets('data class bounding_box', (tester) async {
    final points = [
      const Point(x: 1.0, y: 2.0),
      const Point(x: -3.0, y: 4.0),
      const Point(x: 0.0, y: -1.0),
    ];
    final box = await boundingBox(points: points);
    expect(box.topLeft.x, closeTo(-3.0, 1e-9));
    expect(box.topLeft.y, closeTo(-1.0, 1e-9));
    expect(box.bottomRight.x, closeTo(1.0, 1e-9));
    expect(box.bottomRight.y, closeTo(4.0, 1e-9));
    expect(
      box,
      const Rect(
        topLeft: Point(x: -3.0, y: -1.0),
        bottomRight: Point(x: 1.0, y: 4.0),
      ),
    );
  });

  testWidgets('opaque class Counter create and value', (tester) async {
    final counter = Counter.int32T(initialValue: 10);
    expect(await counter.value(), 10);
    expect(counter.valueSync(), 10);
  });

  testWidgets('opaque class Counter toString (BRIDGE_TO_STRING)', (
    tester,
  ) async {
    final counter = Counter.int32T(initialValue: 42);
    expect(counter.toString(), 'Counter(value: 42)');
    await counter.increment(delta: 1);
    expect('$counter', 'Counter(value: 43)');
  });

  testWidgets('opaque class Counter default constructor', (tester) async {
    final counter = Counter();
    expect(await counter.value(), 0);
  });

  testWidgets('opaque class Counter increment and default delta', (
    tester,
  ) async {
    final counter = Counter.int32T(initialValue: 5);
    await counter.increment();
    expect(await counter.value(), 6);
    await counter.increment(delta: 3);
    expect(await counter.value(), 9);
  });

  testWidgets('opaque class Counter addList', (tester) async {
    final counter = Counter.int32T(initialValue: 10);
    expect(await counter.addList(values: [1, 2, 3]), 16);
    expect(await counter.value(), 16);
  });

  testWidgets('opaque class Counter setValue', (tester) async {
    final counter = Counter.int32T(initialValue: 0);
    await counter.setValue(value: 42);
    expect(await counter.value(), 42);
    await counter.setValue();
    expect(await counter.value(), 42);
  });

  testWidgets('opaque class Counter duplicate', (tester) async {
    final counter = Counter.int32T(initialValue: 7);
    final copy = await counter.duplicate();
    expect(await copy.value(), 7);
    await counter.increment();
    expect(await counter.value(), 8);
    expect(await copy.value(), 7);
  });

  testWidgets('opaque class Counter static sum', (tester) async {
    expect(Counter.sum(a: 3, b: 4), 7);
  });

  testWidgets('opaque class Counter sleepAndGet normal method', (tester) async {
    final counter = Counter.int32T(initialValue: 100);
    expect(await counter.sleepAndGet(sleepMs: 50), 100);
  });

  testWidgets('opaque class Counter greetDartFn', (tester) async {
    final counter = Counter.int32T(initialValue: 5);
    final result = await counter.greetDartFn(
      callback: (value) async => 'Dart got $value',
      name: 'world',
    );
    expect(result, 'hello, Dart got world');
  });

  testWidgets('opaque class Counter tickStream', (tester) async {
    final counter = Counter.int32T(initialValue: 3);
    final values = await counter.tickStream(count: 3, intervalMs: 10).toList();
    expect(values, [3, 3, 3]);
  });

  testWidgets('opaque class Counter dispose then throws', (tester) async {
    final counter = Counter.int32T(initialValue: 1);
    counter.dispose();
    expect(() => counter.valueSync(), throwsA(isA<StateError>()));
  });

  testWidgets('opaque class Counter instances are independent', (tester) async {
    final a = Counter.int32T(initialValue: 1);
    final b = Counter.int32T(initialValue: 2);
    await a.increment();
    expect(await a.value(), 2);
    expect(await b.value(), 2);
    await b.increment(delta: 5);
    expect(await a.value(), 2);
    expect(await b.value(), 7);
  });

  testWidgets('opaque class Counter destructor is called on dispose', (
    tester,
  ) async {
    final baseline = Counter.aliveCount();

    final c1 = Counter.int32T(initialValue: 100);
    final c2 = Counter();
    expect(Counter.aliveCount(), baseline + 2);

    c1.dispose();
    expect(Counter.aliveCount(), baseline + 1);

    c2.dispose();
    expect(Counter.aliveCount(), baseline);
  });

  testWidgets('opaque class Counter double dispose is safe', (tester) async {
    final baseline = Counter.aliveCount();
    final c = Counter.int32T(initialValue: 1);
    expect(Counter.aliveCount(), baseline + 1);

    c.dispose();
    expect(Counter.aliveCount(), baseline);

    c.dispose();
    expect(Counter.aliveCount(), baseline);
  });

  testWidgets(
    'opaque class Counter duplicate creates independent alive count',
    (tester) async {
      final baseline = Counter.aliveCount();
      final original = Counter.int32T(initialValue: 42);
      expect(Counter.aliveCount(), baseline + 1);

      final copy = await original.duplicate();
      expect(Counter.aliveCount(), baseline + 2);

      expect(await copy.value(), 42);
      await original.increment();
      expect(await original.value(), 43);
      expect(await copy.value(), 42);

      original.dispose();
      expect(Counter.aliveCount(), baseline + 1);

      copy.dispose();
      expect(Counter.aliveCount(), baseline);
    },
  );

  // --- Opaque as parameter (free functions) ---

  testWidgets('addCounters sums two Counter values (borrow semantics)', (
    tester,
  ) async {
    final a = Counter.int32T(initialValue: 10);
    final b = Counter.int32T(initialValue: 20);
    final result = await addCounters(a: a, b: b);
    expect(result, 30);
    expect(a.valueSync(), 10);
    expect(b.valueSync(), 20);
    a.dispose();
    b.dispose();
  });

  testWidgets('addCounters with same object twice', (tester) async {
    final c = Counter.int32T(initialValue: 7);
    final result = await addCounters(a: c, b: c);
    expect(result, 14);
    c.dispose();
  });

  testWidgets('cloneWithOffset creates new Counter with offset (sync)', (
    tester,
  ) async {
    final source = Counter.int32T(initialValue: 100);
    final cloned = cloneWithOffset(source: source, offset: 5);
    expect(cloned.valueSync(), 105);
    expect(source.valueSync(), 100);
    source.dispose();
    expect(cloned.valueSync(), 105);
    cloned.dispose();
  });

  testWidgets('cloneWithOffset with negative offset', (tester) async {
    final source = Counter.int32T(initialValue: 3);
    final cloned = cloneWithOffset(source: source, offset: -10);
    expect(cloned.valueSync(), -7);
    source.dispose();
    cloned.dispose();
  });

  testWidgets('addCounters with disposed Counter throws on Dart side', (
    tester,
  ) async {
    final a = Counter.int32T(initialValue: 1);
    final b = Counter.int32T(initialValue: 2);
    a.dispose();
    await expectLater(addCounters(a: a, b: b), throwsA(isA<StateError>()));
    b.dispose();
  });

  testWidgets('cloneWithOffset with disposed source throws on Dart side', (
    tester,
  ) async {
    final source = Counter.int32T(initialValue: 5);
    source.dispose();
    expect(
      () => cloneWithOffset(source: source, offset: 1),
      throwsA(isA<StateError>()),
    );
  });

  testWidgets('cloneWithOffset affects alive count', (tester) async {
    final baseline = Counter.aliveCount();
    final source = Counter.int32T(initialValue: 42);
    expect(Counter.aliveCount(), baseline + 1);
    final cloned = cloneWithOffset(source: source, offset: 0);
    expect(Counter.aliveCount(), baseline + 2);
    source.dispose();
    expect(Counter.aliveCount(), baseline + 1);
    cloned.dispose();
    expect(Counter.aliveCount(), baseline);
  });

  // --- Runtime error propagation tests (R01-R06) ---

  testWidgets('R01: async throw surfaces as StateError', (tester) async {
    await expectLater(
      failAsync(msg: 'boom-async'),
      throwsA(
        isA<StateError>().having(
          (e) => e.message,
          'message',
          contains('boom-async'),
        ),
      ),
    );
  });

  testWidgets('R02: sync throw surfaces as StateError', (tester) async {
    expect(
      () => failSync(msg: 'boom-sync'),
      throwsA(
        isA<StateError>().having(
          (e) => e.message,
          'message',
          contains('boom-sync'),
        ),
      ),
    );
  });

  testWidgets('R03: normal throw surfaces as StateError', (tester) async {
    await expectLater(
      failNormal(msg: 'boom-normal'),
      throwsA(
        isA<StateError>().having(
          (e) => e.message,
          'message',
          contains('boom-normal'),
        ),
      ),
    );
  });

  testWidgets('R04: non-std exception surfaces as unknown', (tester) async {
    await expectLater(
      failNonStd(),
      throwsA(
        isA<StateError>().having(
          (e) => e.message,
          'message',
          contains('unknown'),
        ),
      ),
    );
  });

  testWidgets('R05: stream emits data then error', (tester) async {
    final values = <int>[];
    Object? err;
    try {
      await for (final v in failStream(msg: 'boom-stream')) {
        values.add(v);
      }
    } catch (e) {
      err = e;
    }
    expect(values, [1, 2]);
    expect(err, isA<StateError>());
    expect((err! as StateError).message, contains('boom-stream'));
  });

  testWidgets('R06: session recovers after exception', (tester) async {
    await expectLater(failAsync(msg: 'temp-error'), throwsA(isA<StateError>()));
    expect(await add(a: 10, b: 20), 30);
    expect(bridgeVersion(), 42);
  });

  // --- Deep nesting test (G03) ---

  testWidgets('G03: 3-level nested vector roundtrip', (tester) async {
    final cube = nestedCube(n: 2);
    expect(cube.length, 2);
    expect(cube[0].length, 2);
    expect(cube[0][0].length, 2);
    expect(cube[0][0][0], 0);
    expect(cube[0][0][1], 1);
    expect(cube[0][1][0], 10);
    expect(cube[1][0][0], 100);
    expect(cube[1][1][1], 111);
  });

  testWidgets('G03: nested cube with n=0', (tester) async {
    final cube = nestedCube(n: 0);
    expect(cube, isEmpty);
  });

  // --- time_point <-> DateTime ---

  testWidgets('async echoTime round-trips exact microseconds', (tester) async {
    const epochMicros = 0;
    const y2020Micros = 1577836800000000;
    const withSubSecond = 1577836800123456;
    for (final micros in [epochMicros, y2020Micros, withSubSecond]) {
      final input = DateTime.fromMicrosecondsSinceEpoch(micros, isUtc: true);
      final output = await echoTime(value: input);
      expect(output.microsecondsSinceEpoch, micros);
      expect(output.isUtc, isTrue);
      expect(output, input);
    }
  });

  testWidgets('sync echoTimeSync round-trips exact microseconds', (
    tester,
  ) async {
    const epochMicros = 0;
    const y2020Micros = 1577836800000000;
    const withSubSecond = 1577836800123456;
    for (final micros in [epochMicros, y2020Micros, withSubSecond]) {
      final input = DateTime.fromMicrosecondsSinceEpoch(micros, isUtc: true);
      final output = echoTimeSync(value: input);
      expect(output.microsecondsSinceEpoch, micros);
      expect(output.isUtc, isTrue);
      expect(output, input);
    }
  });

  testWidgets('microsecond precision is preserved (not truncated to millis)', (
    tester,
  ) async {
    const withSubSecond = 1577836800123456;
    final input = DateTime.fromMicrosecondsSinceEpoch(
      withSubSecond,
      isUtc: true,
    );
    final output = await echoTime(value: input);
    expect(output.microsecond, 456);
    expect(output.millisecond, 123);
  });

  testWidgets('result carries no offset (UTC, isUtc == true)', (tester) async {
    const y2020Micros = 1577836800000000;
    final input = DateTime.fromMicrosecondsSinceEpoch(y2020Micros, isUtc: true);
    final output = await echoTime(value: input);
    expect(output.isUtc, isTrue);
    expect(output.timeZoneOffset, Duration.zero);
    expect(output.toIso8601String(), '2020-01-01T00:00:00.000Z');
  });

  // ─── async-simple Signal/Slot cancellation (C01-C06) ──────────────────────
  //
  // Dart cannot forcibly cancel a Future that maps to a running Lazy
  // coroutine. The demo instead keeps a global task_id → Signal map: the
  // coroutine owns a Slot, and cancelTask(taskId) emits
  // SignalType::Terminate. The coroutine's cancellable timer throws
  // async_simple::SignalException, which surfaces in Dart as a StateError.

  testWidgets('C01: task completes normally when never cancelled', (
    tester,
  ) async {
    expect(isTaskRunning(taskId: 'c01'), isFalse);

    final result = await cancellableTask(
      taskId: 'c01',
      steps: 3,
      intervalMs: 5,
    );

    expect(result, 'done:c01');
    expect(isTaskRunning(taskId: 'c01'), isFalse);
    // The registry entry was already removed, so a late cancel is a no-op.
    expect(cancelTask(taskId: 'c01'), isFalse);
  });

  testWidgets('C02: cancel by id makes the Dart Future fail with StateError', (
    tester,
  ) async {
    final future = cancellableTask(
      taskId: 'c02',
      steps: 10000,
      intervalMs: 20,
    );
    await _waitUntil(() => isTaskRunning(taskId: 'c02'));
    expect(isTaskRunning(taskId: 'c02'), isTrue);

    expect(cancelTask(taskId: 'c02'), isTrue);
    await expectLater(
      future,
      throwsA(
        isA<StateError>().having(
          (e) => e.message,
          'message',
          contains('task cancelled by signal'),
        ),
      ),
    );
    expect(isTaskRunning(taskId: 'c02'), isFalse);
    // The signal is one-shot: a second cancel finds nothing to do.
    expect(cancelTask(taskId: 'c02'), isFalse);
  });

  testWidgets('C03: cancellation interrupts the pending timer promptly', (
    tester,
  ) async {
    // Nominal runtime would be 10000 * 50ms = 500s; cancellation must not
    // wait for the current sleep to finish.
    final future = cancellableTask(
      taskId: 'c03',
      steps: 10000,
      intervalMs: 50,
    );
    await _waitUntil(() => isTaskRunning(taskId: 'c03'));
    // Give the coroutine a moment to enter the timer wait.
    await Future<void>.delayed(const Duration(milliseconds: 30));

    final stopwatch = Stopwatch()..start();
    expect(cancelTask(taskId: 'c03'), isTrue);
    await expectLater(future, throwsA(isA<StateError>()));
    stopwatch.stop();

    expect(stopwatch.elapsed, lessThan(const Duration(seconds: 3)));
  });

  testWidgets('C04: cancelling one id leaves other tasks running', (
    tester,
  ) async {
    final victim = cancellableTask(
      taskId: 'c04-victim',
      steps: 10000,
      intervalMs: 20,
    );
    final survivor = cancellableTask(
      taskId: 'c04-survivor',
      steps: 5,
      intervalMs: 10,
    );
    await _waitUntil(
      () =>
          isTaskRunning(taskId: 'c04-victim') &&
          isTaskRunning(taskId: 'c04-survivor'),
    );

    expect(cancelTask(taskId: 'c04-victim'), isTrue);
    await expectLater(victim, throwsA(isA<StateError>()));

    // The other task is unaffected and completes normally.
    expect(await survivor, 'done:c04-survivor');
    expect(isTaskRunning(taskId: 'c04-victim'), isFalse);
    expect(isTaskRunning(taskId: 'c04-survivor'), isFalse);
  });

  testWidgets('C05: unknown ids are safe (no entry, no signal)', (
    tester,
  ) async {
    expect(isTaskRunning(taskId: 'c05-unknown'), isFalse);
    expect(cancelTask(taskId: 'c05-unknown'), isFalse);
  });

  testWidgets('C06: bridge recovers after a cancelled task', (tester) async {
    final future = cancellableTask(
      taskId: 'c06',
      steps: 10000,
      intervalMs: 20,
    );
    await _waitUntil(() => isTaskRunning(taskId: 'c06'));
    expect(cancelTask(taskId: 'c06'), isTrue);
    await expectLater(future, throwsA(isA<StateError>()));

    // Session and runtime are still healthy.
    expect(await add(a: 10, b: 20), 30);
    expect(bridgeVersion(), 42);
  });

  // ─── async-simple collectAll / collectAny (D01-D06) ──────────────────────
  //
  // collectAll waits for every task and returns each result as a Try
  // (exceptions are captured, not thrown). collectAny returns as soon as the
  // first task completes. With SignalType::Terminate, the remaining tasks
  // receive a cancellation signal once one task finishes.

  testWidgets('D01: collectAll waits for all tasks and collects results', (
    tester,
  ) async {
    expect(await collectAllDemo(), '1|two|3');
  });

  testWidgets('D02: collectAllPara sums all results', (tester) async {
    expect(await collectAllParaDemo(), 6);
  });

  testWidgets('D03: collectAll captures task exceptions in Try', (
    tester,
  ) async {
    expect(await collectAllErrorDemo(), 'hello|err-captured');
  });

  testWidgets(
    'D04: collectAll<Terminate> cancels remaining tasks but still waits',
    (tester) async {
      expect(await collectAllCancelDemo(), 'ok|slow-cancelled');
    },
  );

  testWidgets('D05: collectAny returns the first completed task', (
    tester,
  ) async {
    expect(await collectAnyDemo(), 'winner=fast,value=42');
  });

  testWidgets('D06: collectAny<Terminate> cancels the losing task', (
    tester,
  ) async {
    expect(await collectAnyCancelDemo(), 'winner=fast,value=42|loser-cancelled');
  });

  // ══════════════════════════════════════════════════════════════════════════
  // Foreign runtime (libuv + ForeignExecutor) tests
  // ══════════════════════════════════════════════════════════════════════════

  group('libuv foreign runtime', () {
    testWidgets('start and stop uv worker', (tester) async {
      final startResult = await startUvWorker();
      expect(startResult, 'uv worker started');

      final stopResult = await stopUvWorker();
      expect(stopResult, 'uv worker stopped');
    });

    testWidgets('ask_uv: oneshot request/reply via libuv', (tester) async {
      await startUvWorker();

      final result = await askUv(message: 'hello');
      expect(result, '[uv:hello]');

      final result2 = await askUv(message: 'world');
      expect(result2, '[uv:world]');

      await stopUvWorker();
    });

    testWidgets('uv_compute: CPU task on libuv thread', (tester) async {
      await startUvWorker();

      final sum = await uvCompute(n: 100);
      expect(sum, 5050); // 1+2+...+100

      final sum2 = await uvCompute(n: 10);
      expect(sum2, 55);

      await stopUvWorker();
    });

    testWidgets('uv_stream: stream from libuv worker', (tester) async {
      await startUvWorker();

      final items = await uvStream(count: 3, intervalMs: 10).toList();
      expect(items, ['uv_item_0', 'uv_item_1', 'uv_item_2']);

      await stopUvWorker();
    });

    testWidgets('multiple concurrent requests', (tester) async {
      await startUvWorker();

      final results = await Future.wait([
        askUv(message: 'a'),
        askUv(message: 'b'),
        askUv(message: 'c'),
      ]);

      expect(results, ['[uv:a]', '[uv:b]', '[uv:c]']);

      await stopUvWorker();
    });

    testWidgets('error when worker not running', (tester) async {
      await stopUvWorker();

      await expectLater(askUv(message: 'test'), throwsA(isA<Object>()));
    });

    testWidgets('restart worker after stop', (tester) async {
      await startUvWorker();
      final r1 = await askUv(message: 'first');
      expect(r1, '[uv:first]');

      await stopUvWorker();

      await startUvWorker();
      final r2 = await askUv(message: 'second');
      expect(r2, '[uv:second]');

      await stopUvWorker();
    });

    testWidgets('ForeignExecutor sleep resumes normally on the uv loop', (
      tester,
    ) async {
      await startUvWorker();
      try {
        final result = await testForeignSleep();
        expect(result, 'slept');
      } finally {
        await stopUvWorker();
      }
    });

    testWidgets('ForeignExecutor sleep is cancellable via Signal', (
      tester,
    ) async {
      await startUvWorker();
      try {
        final result = await testForeignSleepCancel();
        expect(result, startsWith('cancelled:'));
        expect(result, contains('timer is canceled'));
      } finally {
        await stopUvWorker();
      }
    });

    testWidgets('ForeignExecutor sleep: 30 concurrent native timers', (
      tester,
    ) async {
      await startUvWorker();
      try {
        final results = await Future.wait(
          List.generate(30, (_) => testForeignSleep()),
        );
        expect(results, hasLength(30));
        expect(results.every((r) => r == 'slept'), isTrue);
      } finally {
        await stopUvWorker();
      }
    });

    testWidgets('stop uv worker while a native timer is pending', (
      tester,
    ) async {
      await startUvWorker();
      // Start a 10s sleep and do NOT await it; then stop the worker while the
      // native timer is still live. stop() must clean up the timer on the
      // loop thread and return promptly (this used to hang).
      final pending = testForeignSleepLong();
      await Future<void>.delayed(const Duration(milliseconds: 100));

      final stopwatch = Stopwatch()..start();
      await stopUvWorker();
      stopwatch.stop();
      pending.ignore();

      expect(stopwatch.elapsed, lessThan(const Duration(seconds: 3)));

      // A fresh worker still works afterwards.
      await startUvWorker();
      try {
        expect(await testForeignSleep(), 'slept');
      } finally {
        await stopUvWorker();
      }
    });
  });

  group('DartFn from libuv foreign runtime', () {
    setUpAll(() async {
      await startUvWorker();
    });

    tearDownAll(() async {
      await stopUvWorker();
    });

    testWidgets('call Dart callback from libuv loop (ForeignExecutor)', (
      tester,
    ) async {
      final result = await callDartFromUv(
        callback: (s) async => 'uv-echo:$s',
        input: 'hello',
      );
      expect(result, 'uv-echo:hello');
    });

    testWidgets('multiple sequential calls from libuv', (tester) async {
      final r1 = await callDartFromUv(
        callback: (s) async => s.toUpperCase(),
        input: 'abc',
      );
      final r2 = await callDartFromUv(
        callback: (s) async => s.toUpperCase(),
        input: 'xyz',
      );
      expect(r1, 'ABC');
      expect(r2, 'XYZ');
    });

    testWidgets('Dart callback with async delay from libuv', (tester) async {
      final result = await callDartFromUv(
        callback: (s) async {
          await Future.delayed(Duration(milliseconds: 30));
          return 'delayed:$s';
        },
        input: 'wait',
      );
      expect(result, 'delayed:wait');
    });

    testWidgets('Dart callback that throws returns error from libuv', (
      tester,
    ) async {
      final result = await callDartFromUv(
        callback: (s) async => throw StateError('uv-boom-$s'),
        input: 'err',
      );
      expect(result, startsWith('ERROR:'));
    });
  });

  group('cbridge pure C API', () {
    testWidgets('dcb_async_create + dcb_async_complete + async_wait', (
      tester,
    ) async {
      final result = await testCbridgeAsync();
      expect(result, 'cbridge_ok');
    });

    testWidgets('dcb_async_fail propagates error to coroutine', (tester) async {
      final result = await testCbridgeAsyncFail();
      expect(result, 'CAUGHT:intentional_error');
    });

    testWidgets('dcb_async_cancel propagates cancellation to coroutine', (
      tester,
    ) async {
      final result = await testCbridgeAsyncCancel();
      expect(result, 'CAUGHT:async_wait: operation cancelled');
    });

    testWidgets('channel service: mpsc long-lived service on uv worker', (
      tester,
    ) async {
      await startUvWorker();

      final result = await testChannelService();
      expect(result, '[svc:msg0],[svc:msg1],[svc:msg2]');

      await stopUvWorker();
    });

    testWidgets('channel service concurrent: batch send then collect replies', (
      tester,
    ) async {
      await startUvWorker();

      final result = await testChannelServiceConcurrent();
      expect(result, '[svc:c0],[svc:c1],[svc:c2],[svc:c3],[svc:c4]');

      await stopUvWorker();
    });

    testWidgets('dcb_invoke_dart_fn: C callback-style DartFn invocation', (
      tester,
    ) async {
      final result = await testCbridgeInvoke(
        callback: (s) async => 'c-echo:$s',
        input: 'hello',
      );
      expect(result, 'c-echo:hello');
    });

    testWidgets('dcb_invoke_dart_fn: Dart callback with delay', (tester) async {
      final result = await testCbridgeInvoke(
        callback: (s) async {
          await Future.delayed(Duration(milliseconds: 30));
          return 'delayed:$s';
        },
        input: 'wait',
      );
      expect(result, 'delayed:wait');
    });

    testWidgets('dcb_invoke_dart_fn: Dart callback that throws', (
      tester,
    ) async {
      final result = await testCbridgeInvoke(
        callback: (s) async => throw StateError('boom-$s'),
        input: 'err',
      );
      expect(result, startsWith('ERROR:'));
    });

    testWidgets('pure C invoke: dcb_async_create + C callback + async_wait', (
      tester,
    ) async {
      final result = await testCbridgeInvokePureC(
        callback: (s) async => 'dart-pure:$s',
        input: 'hello',
      );
      // C layer prepends the "C:" prefix
      expect(result, 'C:dart-pure:hello');
    });

    testWidgets('pure C invoke: Dart callback with delay', (tester) async {
      final result = await testCbridgeInvokePureC(
        callback: (s) async {
          await Future.delayed(Duration(milliseconds: 30));
          return 'delayed:$s';
        },
        input: 'wait',
      );
      expect(result, 'C:delayed:wait');
    });

    testWidgets('pure C invoke: Dart callback that throws', (tester) async {
      // dcb_async_fail propagates error → coroutine throws runtime_error → Dart receives exception
      expect(
        () => testCbridgeInvokePureC(
          callback: (s) async => throw StateError('boom-$s'),
          input: 'err',
        ),
        throwsA(anything),
      );
    });

    testWidgets('pure C cancel: dcb_async_cancel from C thread', (
      tester,
    ) async {
      final result = await testCbridgePureCCancel();
      expect(result, startsWith('CAUGHT:'));
      expect(result, contains('cancel'));
    });
  });

  // ─── Multi-runtime communication tests ───────────────────────────────────

  group('worker lifecycle', () {
    testWidgets('start workers', (tester) async {
      final result = await startWorkers();
      expect(result, 'workers started');
    });

    testWidgets('stop workers', (tester) async {
      final result = await stopWorkers();
      expect(result, 'workers stopped');
    });

    testWidgets('restart workers', (tester) async {
      final r1 = await startWorkers();
      expect(r1, 'workers started');
      final r2 = await stopWorkers();
      expect(r2, 'workers stopped');
    });
  });

  group('cross-runtime communication', () {
    setUpAll(() async {
      await startWorkers();
    });

    tearDownAll(() async {
      await stopWorkers();
    });

    testWidgets('process message via Worker A (oneshot channel)', (
      tester,
    ) async {
      final result = await processMessage(message: 'hello');
      expect(result, '[A:hello]');
    });

    testWidgets('process multiple messages sequentially', (tester) async {
      final r1 = await processMessage(message: 'foo');
      final r2 = await processMessage(message: 'bar');
      expect(r1, '[A:foo]');
      expect(r2, '[A:bar]');
    });

    testWidgets('ping Worker B (oneshot request/reply)', (tester) async {
      final result = await pingWorker(payload: 'test');
      expect(result, matches(RegExp(r'^\[B#\d+:test\]$')));
    });

    testWidgets('ping counter increments', (tester) async {
      final r1 = await pingWorker(payload: 'a');
      final r2 = await pingWorker(payload: 'b');
      // Extract counter values
      final id1 = int.parse(RegExp(r'#(\d+)').firstMatch(r1)!.group(1)!);
      final id2 = int.parse(RegExp(r'#(\d+)').firstMatch(r2)!.group(1)!);
      expect(id2, greaterThan(id1));
    });

    testWidgets('pipeline: Worker A → Worker B', (tester) async {
      final result = await pipeline(message: 'data');
      // Worker A wraps: A{data}, Worker B wraps: B[A{data}]
      expect(result, 'B[A{data}]');
    });

    testWidgets('fan-out: both workers reply', (tester) async {
      final (a, b) = await fanOut(message: 'msg');
      expect(a, 'A:msg');
      expect(b, 'B:msg');
    });

    testWidgets('concurrent requests to different workers', (tester) async {
      // Fire multiple requests concurrently — all should resolve.
      final futures = [
        processMessage(message: 'x1'),
        pingWorker(payload: 'x2'),
        processMessage(message: 'x3'),
        pingWorker(payload: 'x4'),
      ];
      final results = await Future.wait(futures);
      expect(results[0], '[A:x1]');
      expect(results[1], matches(RegExp(r'^\[B#\d+:x2\]$')));
      expect(results[2], '[A:x3]');
      expect(results[3], matches(RegExp(r'^\[B#\d+:x4\]$')));
    });
  });

  group('worker stream (mpsc channel)', () {
    setUpAll(() async {
      await startWorkers();
    });

    tearDownAll(() async {
      await stopWorkers();
    });

    testWidgets('receives all items from Worker A stream', (tester) async {
      final items = await workerStream(count: 5, intervalMs: 10).toList();
      expect(items, hasLength(5));
      expect(items[0], 'item_0');
      expect(items[4], 'item_4');
    });

    testWidgets('stream with zero items completes immediately', (
      tester,
    ) async {
      final items = await workerStream(count: 0, intervalMs: 0).toList();
      expect(items, isEmpty);
    });

    testWidgets('multiple streams can run sequentially', (tester) async {
      final items1 = await workerStream(count: 3, intervalMs: 5).toList();
      final items2 = await workerStream(count: 2, intervalMs: 5).toList();
      expect(items1, hasLength(3));
      expect(items2, hasLength(2));
    });
  });

  group('DartFn from worker runtime', () {
    setUpAll(() async {
      await startWorkers();
    });

    tearDownAll(() async {
      await stopWorkers();
    });

    testWidgets('call Dart callback from Worker A (independent AsioExecutor)', (
      tester,
    ) async {
      final result = await callDartFromWorkerA(
        callback: (s) async => 'dart-echo:$s',
        input: 'hello',
      );
      expect(result, 'dart-echo:hello');
    });

    testWidgets('call Dart callback from Worker B (independent AsioExecutor)', (
      tester,
    ) async {
      final result = await callDartFromWorkerB(
        callback: (s) async => 'B:$s',
        input: 'world',
      );
      expect(result, 'B:world');
    });

    testWidgets('multiple sequential DartFn calls from Worker A', (
      tester,
    ) async {
      final r1 = await callDartFromWorkerA(
        callback: (s) async => s.toUpperCase(),
        input: 'abc',
      );
      final r2 = await callDartFromWorkerA(
        callback: (s) async => s.toUpperCase(),
        input: 'def',
      );
      expect(r1, 'ABC');
      expect(r2, 'DEF');
    });

    testWidgets('concurrent DartFn calls from both workers', (tester) async {
      final (a, b) = await (
        callDartFromWorkerA(
          callback: (s) async => 'fromA:$s',
          input: 'x',
        ),
        callDartFromWorkerB(
          callback: (s) async => 'fromB:$s',
          input: 'y',
        ),
      ).wait;
      expect(a, 'fromA:x');
      expect(b, 'fromB:y');
    });

    testWidgets('Dart callback with async work (delay)', (tester) async {
      final result = await callDartFromWorkerA(
        callback: (s) async {
          await Future.delayed(Duration(milliseconds: 50));
          return 'delayed:$s';
        },
        input: 'wait',
      );
      expect(result, 'delayed:wait');
    });

    testWidgets('Dart callback that throws returns error', (tester) async {
      final result = await callDartFromWorkerA(
        callback: (s) async => throw StateError('boom-$s'),
        input: 'err',
      );
      // Worker catches the exception and returns ERROR: prefix
      expect(result, startsWith('ERROR:'));
    });
  });

  // ─── Stress / edge-case tests ─────────────────────────────────────────────

  group('stress: concurrent async calls', () {
    testWidgets('100 concurrent add() calls', (tester) async {
      final futures = List.generate(
        100,
        (i) => add(a: i, b: i * 2),
      );
      final results = await Future.wait(futures);
      for (var i = 0; i < 100; i++) {
        expect(results[i], i + i * 2);
      }
    });

    testWidgets('50 concurrent processMessage via workers', (tester) async {
      await startWorkers();
      try {
        final futures = List.generate(
          50,
          (i) => processMessage(message: 'm$i'),
        );
        final results = await Future.wait(futures);
        for (var i = 0; i < 50; i++) {
          expect(results[i], '[A:m$i]');
        }
      } finally {
        await stopWorkers();
      }
    });

    testWidgets('50 concurrent pipeline (A→B)', (tester) async {
      await startWorkers();
      try {
        final futures = List.generate(
          50,
          (i) => pipeline(message: 'p$i'),
        );
        final results = await Future.wait(futures);
        for (var i = 0; i < 50; i++) {
          expect(results[i], 'B[A{p$i}]');
        }
      } finally {
        await stopWorkers();
      }
    });
  });

  group('stress: large payload', () {
    testWidgets('1MB string round-trip via processMessage', (tester) async {
      await startWorkers();
      try {
        final big = 'x' * (1024 * 1024); // 1MB
        final result = await processMessage(message: big);
        expect(result.length, big.length + 4); // "[A:" + big + "]"
        expect(result, '[A:$big]');
      } finally {
        await stopWorkers();
      }
    });

    testWidgets('large list round-trip via echoList', (tester) async {
      final big = List.generate(100000, (i) => i);
      final result = await echoList(values: big);
      expect(result.length, 100000);
      expect(result.first, 0);
      expect(result.last, 99999);
    });
  });

  group('stress: stream cancel + resubscribe', () {
    testWidgets('cancel tickStream early then resubscribe', (tester) async {
      // First subscription: cancel after 2 items
      final items1 = <int>[];
      final sub1 = tickStream(count: 100, intervalMs: 5).listen(items1.add);
      await Future.delayed(Duration(milliseconds: 80));
      await sub1.cancel();
      expect(items1.length, lessThan(100));
      expect(items1.length, greaterThan(0));

      // Second subscription: should work fine
      final items2 = await tickStream(count: 5, intervalMs: 5).toList();
      expect(items2, [0, 1, 2, 3, 4]);
    });

    testWidgets('cancel workerStream early then resubscribe', (tester) async {
      await startWorkers();
      try {
        // Cancel early
        final items1 = <String>[];
        final sub = workerStream(count: 100, intervalMs: 10).listen(items1.add);
        await Future.delayed(Duration(milliseconds: 100));
        await sub.cancel();
        expect(items1.length, lessThan(100));

        // Resubscribe: full run
        final items2 = await workerStream(count: 3, intervalMs: 10).toList();
        expect(items2.length, 3);
      } finally {
        await stopWorkers();
      }
    });

    testWidgets('multiple streams sequentially', (tester) async {
      for (var round = 0; round < 5; round++) {
        final items = await tickStream(count: 3, intervalMs: 1).toList();
        expect(items, [0, 1, 2], reason: 'round $round');
      }
    });
  });

  group('stress: concurrent DartFn callbacks', () {
    testWidgets('10 concurrent DartFn from Worker A', (tester) async {
      await startWorkers();
      try {
        final futures = List.generate(
          10,
          (i) => callDartFromWorkerA(
            callback: (s) async => 'cb$i:$s',
            input: 'in$i',
          ),
        );
        final results = await Future.wait(futures);
        for (var i = 0; i < 10; i++) {
          expect(results[i], 'cb$i:in$i');
        }
      } finally {
        await stopWorkers();
      }
    });

    testWidgets('10 concurrent DartFn split across A and B', (tester) async {
      await startWorkers();
      try {
        final futuresA = List.generate(
          5,
          (i) => callDartFromWorkerA(
            callback: (s) async => 'A$i:$s',
            input: 'a$i',
          ),
        );
        final futuresB = List.generate(
          5,
          (i) => callDartFromWorkerB(
            callback: (s) async => 'B$i:$s',
            input: 'b$i',
          ),
        );
        final results = await Future.wait([...futuresA, ...futuresB]);
        for (var i = 0; i < 5; i++) {
          expect(results[i], 'A$i:a$i');
        }
        for (var i = 0; i < 5; i++) {
          expect(results[5 + i], 'B$i:b$i');
        }
      } finally {
        await stopWorkers();
      }
    });

    testWidgets('DartFn with async delay under concurrency', (tester) async {
      await startWorkers();
      try {
        final futures = List.generate(
          10,
          (i) => callDartFromWorkerA(
            callback: (s) async {
              await Future.delayed(Duration(milliseconds: 10));
              return 'delayed$i:$s';
            },
            input: 'x$i',
          ),
        );
        final results = await Future.wait(futures);
        for (var i = 0; i < 10; i++) {
          expect(results[i], 'delayed$i:x$i');
        }
      } finally {
        await stopWorkers();
      }
    });
  });

  // ─── Error paths: worker not started ─────────────────────────────────────

  group('error: worker not started', () {
    // Ensure workers are stopped before this group
    setUpAll(() async {
      // Stop if running from previous group
      try {
        await stopWorkers();
      } catch (_) {}
    });

    testWidgets('processMessage throws when workers not running', (
      tester,
    ) async {
      expect(
        () => processMessage(message: 'hello'),
        throwsA(anything),
      );
    });

    testWidgets('pingWorker throws when workers not running', (
      tester,
    ) async {
      expect(
        () => pingWorker(payload: 'ping'),
        throwsA(anything),
      );
    });

    testWidgets('pipeline throws when workers not running', (
      tester,
    ) async {
      expect(
        () => pipeline(message: 'data'),
        throwsA(anything),
      );
    });

    testWidgets('fanOut throws when workers not running', (
      tester,
    ) async {
      expect(
        () => fanOut(message: 'hello'),
        throwsA(anything),
      );
    });

    testWidgets('callDartFromWorkerA throws when workers not running', (
      tester,
    ) async {
      expect(
        () => callDartFromWorkerA(
          callback: (s) async => s,
          input: 'x',
        ),
        throwsA(anything),
      );
    });

    testWidgets('callDartFromWorkerB throws when workers not running', (
      tester,
    ) async {
      expect(
        () => callDartFromWorkerB(
          callback: (s) async => s,
          input: 'x',
        ),
        throwsA(anything),
      );
    });

    testWidgets('workerStream emits error when workers not running', (
      tester,
    ) async {
      expect(
        () => workerStream(count: 3, intervalMs: 10).toList(),
        throwsA(anything),
      );
    });

    testWidgets('all 50 concurrent calls fail gracefully', (tester) async {
      final futures = List.generate(
        50,
        (i) => processMessage(message: 'm$i').then(
          (_) => 'ok',
          onError: (_) => 'error',
        ),
      );
      final results = await Future.wait(futures);
      // All should be errors
      expect(results.every((r) => r == 'error'), isTrue);
    });
  });

  // ─── Stress: rapid init/dispose cycles ────────────────────────────────────

  group('stress: rapid init/dispose cycles', () {
    testWidgets('20 rapid init/dispose cycles with sync call', (
      tester,
    ) async {
      // Release the global bridge
      DcbLib.dispose();

      for (var i = 0; i < 20; i++) {
        await DcbLib.init();
        expect(DcbLib.isInitialized, isTrue);
        // Verify the bridge actually works
        final v = bridgeVersion();
        expect(v, greaterThan(0));
        DcbLib.dispose();
        expect(DcbLib.isInitialized, isFalse);
      }

      // Re-init for tearDownAll shutdown
      await DcbLib.init();
    });

    testWidgets('10 cycles with async call in between', (tester) async {
      DcbLib.dispose();

      for (var i = 0; i < 10; i++) {
        await DcbLib.init();
        final result = await add(a: i, b: 1);
        expect(result, i + 1);
        DcbLib.dispose();
      }

      await DcbLib.init();
    });

    testWidgets('5 cycles with DartFn callback', (tester) async {
      DcbLib.dispose();

      for (var i = 0; i < 5; i++) {
        await DcbLib.init();
        final result = await greetDartFn(
          callback: (name) async => 'hi $name ($i)',
          name: 'cycle',
        );
        expect(result, 'hello, hi cycle ($i)');
        DcbLib.dispose();
      }

      await DcbLib.init();
    });
  });
}
