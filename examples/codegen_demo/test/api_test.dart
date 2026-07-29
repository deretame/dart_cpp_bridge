import 'dart:async';

import 'package:codegen_demo/codegen_demo.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  setUpAll(() async {
    await DcbLib.init();
  });

  tearDownAll(() {
    DcbLib.shutdown();
  });

  test('BRIDGE_SYNC bridge_version', () {
    expect(bridgeVersion(), 42);
  });

  test('BRIDGE_ASYNC add', () async {
    expect(await add(a: 2, b: 3), 5);
  });

  test('BRIDGE_NORMAL sleep_greeting', () async {
    expect(await sleepGreeting(name: 'Ada'), 'hello, Ada');
  });

  test('BRIDGE_ASYNC enum next_status', () async {
    expect(await nextStatus(current: OrderStatus.created), OrderStatus.paid);
    expect(await nextStatus(current: OrderStatus.paid), OrderStatus.shipped);
    expect(await nextStatus(current: OrderStatus.shipped), OrderStatus.created);
  });

  test('BRIDGE_ASYNC optional maybe_double', () async {
    expect(await maybeDouble(), isNull);
    expect(await maybeDouble(value: 5), 10);
    expect(await maybeDouble(value: -3), -6);
  });

  test('BRIDGE_ASYNC u32 increment_u32', () async {
    expect(await incrementU32(value: 0), 1);
    expect(await incrementU32(value: 4294967290), 4294967291);
  });

  test('BRIDGE_ASYNC i64 increment_i64', () async {
    expect(await incrementI64(value: 0), 1);
    expect(await incrementI64(value: 9223372036854775800), 9223372036854775801);
    expect(await incrementI64(value: -9223372036854775800), -9223372036854775799);
  });

  test('BRIDGE_ASYNC bool negate_bool', () async {
    expect(await negateBool(value: true), false);
    expect(await negateBool(value: false), true);
  });

  test('BRIDGE_ASYNC optional string', () async {
    expect(await optionalString(), isNull);
    expect(await optionalString(value: 'hello'), 'hello!');
  });

  test('BRIDGE_ASYNC optional enum', () async {
    expect(await optionalStatus(), isNull);
    expect(await optionalStatus(value: OrderStatus.created), OrderStatus.paid);
    expect(await optionalStatus(value: OrderStatus.shipped), OrderStatus.created);
  });

  test('BRIDGE_ASYNC vector<int> echo_list', () async {
    expect(await echoList(values: []), <int>[]);
    expect(await echoList(values: [1, 2, 3]), [1, 2, 3]);
    expect(await echoList(values: [-1, 0, 42]), [-1, 0, 42]);
  });

  test('BRIDGE_ASYNC array<int, 4> sum_array', () async {
    expect(await sumArray(values: [1, 2, 3, 4]), 10);
    expect(await sumArray(values: [-1, 1, -1, 1]), 0);
  });

  test('BRIDGE_ASYNC map<string, int> sum_scores', () async {
    expect(await sumScores(scores: {}), 0);
    expect(await sumScores(scores: {'a': 1, 'b': 2, 'c': 3}), 6);
  });

  test('BRIDGE_ASYNC set<int> sum_set', () async {
    expect(await sumSet(values: <int>{}), 0);
    expect(await sumSet(values: {1, 2, 3}), 6);
  });

  test('BRIDGE_ASYNC Int128 echo_i128', () async {
    final big = BigInt.parse('170141183460469231731687303715884105727');
    expect(await echoI128(value: big), big);
    expect(await echoI128(value: BigInt.zero), BigInt.zero);
    expect(
      await echoI128(value: BigInt.parse('-170141183460469231731687303715884105728')),
      BigInt.parse('-170141183460469231731687303715884105728'),
    );
  });

  test('BRIDGE_ASYNC UInt128 echo_u128', () async {
    final big = BigInt.parse('340282366920938463463374607431768211455');
    expect(await echoU128(value: big), big);
    expect(await echoU128(value: BigInt.zero), BigInt.zero);
  });

  test('BRIDGE_ASYNC DartFn greet_dart_fn', () async {
    expect(
      await greetDartFn(callback: (name) async => 'Dart $name', name: 'world'),
      'hello, Dart world',
    );
    expect(
      await greetDartFn(callback: (name) async {
        await Future<void>.delayed(const Duration(milliseconds: 10));
        return 'async $name';
      }, name: 'moon'),
      'hello, async moon',
    );
  });

  test('BRIDGE_NORMAL DartFn syncAwait with two args', () async {
    expect(
      await concatDartFn(callback: (a, b) async => '$a+$b', a: 'foo', b: 'bar'),
      'sync:foo+bar',
    );
    expect(
      await concatDartFn(callback: (a, b) async => '$b-$a', a: 'X', b: 'Y'),
      'sync:Y-X',
    );
  });

  test('FRB-style: sync register + async invoke (no deadlock)', () async {
    // register_dart_fn is BRIDGE_SYNC — runs on isolate thread, just stores.
    final ok = registerDartFn(callback: (s) async => 'echo:$s');
    expect(ok, isTrue);

    // invoke_registered is BRIDGE_NORMAL — runs on pool thread, calls Dart.
    final result = await invokeRegistered(input: 'world');
    expect(result, 'registered:echo:world');

    // Re-register with a different closure and invoke again.
    registerDartFn(callback: (s) async => s.toUpperCase());
    final result2 = await invokeRegistered(input: 'hello');
    expect(result2, 'registered:HELLO');
  });

  test('FRB-style: sync register + coroutine invoke (co_await fn(...))', () async {
    // Same registration (BRIDGE_SYNC, just stores).
    registerDartFn(callback: (s) async => 'co:$s');

    // invoke_registered_async is BRIDGE_ASYNC — co_await fn(...) on io thread.
    final result = await invokeRegisteredAsync(input: 'coroutine');
    expect(result, 'async_registered:co:coroutine');

    // Re-register and invoke again to prove reusability.
    registerDartFn(callback: (s) async => '${s.length}');
    final result2 = await invokeRegisteredAsync(input: 'abcd');
    expect(result2, 'async_registered:4');
  });

  test('BRIDGE_ASYNC pair<int, string> pair_echo', () async {
    expect(await pairEcho(value: (1, 'hello')), (1, 'hello'));
    expect(await pairEcho(value: (-42, 'world')), (-42, 'world'));
  });

  test('BRIDGE_ASYNC tuple<int, string, bool> tuple_echo', () async {
    expect(await tupleEcho(value: (1, 'hello', true)), (1, 'hello', true));
    expect(await tupleEcho(value: (-42, 'world', false)), (-42, 'world', false));
  });

  test('Stream tick_stream emits 0..count-1 then done', () async {
    final values = await tickStream(count: 5, intervalMs: 10).toList();
    expect(values, [0, 1, 2, 3, 4]);
  });

  test('Stream tick_stream cancels subscription', () async {
    final stream = tickStream(count: 100, intervalMs: 10);
    final sub = stream.listen(null);
    await Future<void>.delayed(const Duration(milliseconds: 30));
    await sub.cancel();
  });

  test('optional StreamSink downloadWithProgress with progress', () async {
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

  test('optional StreamSink downloadWithProgress without progress', () async {
    final result = await downloadWithProgress(url: 'test.txt');
    expect(result, 'downloaded: test.txt');
  });

  test('data class distance', () async {
    final a = const Point(x: 0.0, y: 0.0);
    final b = const Point(x: 3.0, y: 4.0);
    expect(await distance(a: a, b: b), closeTo(5.0, 1e-9));
    expect(await distance(a: a, b: a), closeTo(0.0, 1e-9));
  });

  test('data class toString (default + custom dart_code)', () {
    const p = Point(x: 1.0, y: 2.0);
    expect(p.toString(), 'Point(x: 1.0, y: 2.0, label: null)');
    const r = Rect(
      topLeft: Point(x: 0.0, y: 0.0),
      bottomRight: Point(x: 3.0, y: 4.0),
    );
    expect(r.toString(), 'Rect[Point(x: 0.0, y: 0.0, label: null) -> Point(x: 3.0, y: 4.0, label: null)]');
  });

  test('data class scale', () async {
    final p = const Point(x: 1.5, y: -2.0);
    final scaled = await scale(p: p, factor: 2.0);
    expect(scaled.x, closeTo(3.0, 1e-9));
    expect(scaled.y, closeTo(-4.0, 1e-9));
    expect(scaled.label, isNull);
  });

  test('data class optional field round-trip', () async {
    final p = const Point(x: 1.0, y: 2.0, label: 'hello');
    final scaled = await scale(p: p, factor: 3.0);
    expect(scaled.x, closeTo(3.0, 1e-9));
    expect(scaled.y, closeTo(6.0, 1e-9));
    expect(scaled.label, 'hello');
  });

  test('data class bounding_box', () async {
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

  test('opaque class Counter create and value', () async {
    final counter = Counter.int32T(initialValue: 10);
    expect(await counter.value(), 10);
    expect(counter.valueSync(), 10);
  });

  test('opaque class Counter toString (BRIDGE_TO_STRING)', () async {
    final counter = Counter.int32T(initialValue: 42);
    expect(counter.toString(), 'Counter(value: 42)');
    await counter.increment(delta: 1);
    expect('$counter', 'Counter(value: 43)');
  });

  test('opaque class Counter default constructor', () async {
    final counter = Counter();
    expect(await counter.value(), 0);
  });

  test('opaque class Counter increment and default delta', () async {
    final counter = Counter.int32T(initialValue: 5);
    await counter.increment();
    expect(await counter.value(), 6);
    await counter.increment(delta: 3);
    expect(await counter.value(), 9);
  });

  test('opaque class Counter addList', () async {
    final counter = Counter.int32T(initialValue: 10);
    expect(await counter.addList(values: [1, 2, 3]), 16);
    expect(await counter.value(), 16);
  });

  test('opaque class Counter setValue', () async {
    final counter = Counter.int32T(initialValue: 0);
    await counter.setValue(value: 42);
    expect(await counter.value(), 42);
    await counter.setValue();
    expect(await counter.value(), 42);
  });

  test('opaque class Counter duplicate', () async {
    final counter = Counter.int32T(initialValue: 7);
    final copy = await counter.duplicate();
    expect(await copy.value(), 7);
    await counter.increment();
    expect(await counter.value(), 8);
    expect(await copy.value(), 7);
  });

  test('opaque class Counter static sum', () {
    expect(Counter.sum(a: 3, b: 4), 7);
  });

  test('opaque class Counter sleepAndGet normal method', () async {
    final counter = Counter.int32T(initialValue: 100);
    expect(await counter.sleepAndGet(sleepMs: 50), 100);
  });

  test('opaque class Counter greetDartFn', () async {
    final counter = Counter.int32T(initialValue: 5);
    final result = await counter.greetDartFn(
      callback: (value) async => 'Dart got $value',
      name: 'world',
    );
    expect(result, 'hello, Dart got world');
  });

  test('opaque class Counter tickStream', () async {
    final counter = Counter.int32T(initialValue: 3);
    final values = await counter.tickStream(count: 3, intervalMs: 10).toList();
    expect(values, [3, 3, 3]);
  });

  test('opaque class Counter dispose then throws', () async {
    final counter = Counter.int32T(initialValue: 1);
    counter.dispose();
    expect(() => counter.valueSync(), throwsA(isA<StateError>()));
  });

  test('opaque class Counter instances are independent', () async {
    final a = Counter.int32T(initialValue: 1);
    final b = Counter.int32T(initialValue: 2);
    await a.increment();
    expect(await a.value(), 2);
    expect(await b.value(), 2);
    await b.increment(delta: 5);
    expect(await a.value(), 2);
    expect(await b.value(), 7);
  });

  test('opaque class Counter destructor is called on dispose', () async {
    // Record baseline alive count (other tests may have leaked counters).
    final baseline = Counter.aliveCount();

    final c1 = Counter.int32T(initialValue: 100);
    final c2 = Counter();
    expect(Counter.aliveCount(), baseline + 2);

    // Dispose c1 explicitly — C++ destructor should run immediately.
    c1.dispose();
    expect(Counter.aliveCount(), baseline + 1);

    // Dispose c2.
    c2.dispose();
    expect(Counter.aliveCount(), baseline);
  });

  test('opaque class Counter double dispose is safe', () {
    final baseline = Counter.aliveCount();
    final c = Counter.int32T(initialValue: 1);
    expect(Counter.aliveCount(), baseline + 1);

    c.dispose();
    expect(Counter.aliveCount(), baseline);

    // Second dispose should be a no-op (idempotent).
    c.dispose();
    expect(Counter.aliveCount(), baseline);
  });

  test(
    'opaque class Counter duplicate creates independent alive count',
    () async {
      final baseline = Counter.aliveCount();
      final original = Counter.int32T(initialValue: 42);
      expect(Counter.aliveCount(), baseline + 1);

      final copy = await original.duplicate();
      expect(Counter.aliveCount(), baseline + 2);

      // They are independent objects.
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

  group('opaque as parameter (codegen free functions)', () {
    test('addCounters sums two Counter values (borrow semantics)', () async {
      final a = Counter.int32T(initialValue: 10);
      final b = Counter.int32T(initialValue: 20);
      final result = await addCounters(a: a, b: b);
      expect(result, 30);
      // Both still usable after borrow.
      expect(a.valueSync(), 10);
      expect(b.valueSync(), 20);
      a.dispose();
      b.dispose();
    });

    test('addCounters with same object twice', () async {
      final c = Counter.int32T(initialValue: 7);
      final result = await addCounters(a: c, b: c);
      expect(result, 14);
      c.dispose();
    });

    test('cloneWithOffset creates new Counter with offset (sync)', () {
      final source = Counter.int32T(initialValue: 100);
      final cloned = cloneWithOffset(source: source, offset: 5);
      expect(cloned.valueSync(), 105);
      // Source unchanged.
      expect(source.valueSync(), 100);
      // They are independent.
      source.dispose();
      expect(cloned.valueSync(), 105);
      cloned.dispose();
    });

    test('cloneWithOffset with negative offset', () {
      final source = Counter.int32T(initialValue: 3);
      final cloned = cloneWithOffset(source: source, offset: -10);
      expect(cloned.valueSync(), -7);
      source.dispose();
      cloned.dispose();
    });

    test('addCounters with disposed Counter throws on Dart side', () async {
      final a = Counter.int32T(initialValue: 1);
      final b = Counter.int32T(initialValue: 2);
      a.dispose();
      await expectLater(
        addCounters(a: a, b: b),
        throwsA(isA<StateError>()),
      );
      b.dispose();
    });

    test('cloneWithOffset with disposed source throws on Dart side', () {
      final source = Counter.int32T(initialValue: 5);
      source.dispose();
      expect(
        () => cloneWithOffset(source: source, offset: 1),
        throwsA(isA<StateError>()),
      );
    });

    test('cloneWithOffset affects alive count', () {
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
  });

  // --- Runtime error propagation tests (R01-R06) ---

  group('runtime error propagation', () {
    test('R01: async throw surfaces as StateError', () async {
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

    test('R02: sync throw surfaces as StateError', () {
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

    test('R03: normal throw surfaces as StateError', () async {
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

    test('R04: non-std exception surfaces as unknown', () async {
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

    test('R05: stream emits data then error', () async {
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

    test('R06: session recovers after exception', () async {
      // Trigger an exception first.
      await expectLater(failAsync(msg: 'temp-error'), throwsA(isA<StateError>()));
      // Session should still work normally.
      expect(await add(a: 10, b: 20), 30);
      expect(bridgeVersion(), 42);
    });
  });

  // --- Deep nesting test (G03) ---

  group('deep nesting containers', () {
    test('G03: 3-level nested vector roundtrip', () {
      final cube = nestedCube(n: 2);
      expect(cube.length, 2);
      expect(cube[0].length, 2);
      expect(cube[0][0].length, 2);
      // cube[i][j][k] = i*100 + j*10 + k
      expect(cube[0][0][0], 0);
      expect(cube[0][0][1], 1);
      expect(cube[0][1][0], 10);
      expect(cube[1][0][0], 100);
      expect(cube[1][1][1], 111);
    });

    test('G03: nested cube with n=0', () {
      final cube = nestedCube(n: 0);
      expect(cube, isEmpty);
    });
  });

  group('time_point <-> DateTime (Unix micros, UTC)', () {
    // Reference instants expressed as microseconds since the Unix epoch.
    const epochMicros = 0; // 1970-01-01T00:00:00Z
    const y2020Micros = 1577836800000000; // 2020-01-01T00:00:00Z
    const withSubSecond = 1577836800123456; // +123456us microsecond precision

    test('async echoTime round-trips exact microseconds', () async {
      for (final micros in [epochMicros, y2020Micros, withSubSecond]) {
        final input = DateTime.fromMicrosecondsSinceEpoch(micros, isUtc: true);
        final output = await echoTime(value: input);
        expect(output.microsecondsSinceEpoch, micros);
        expect(output.isUtc, isTrue);
        expect(output, input);
      }
    });

    test('sync echoTimeSync round-trips exact microseconds', () {
      for (final micros in [epochMicros, y2020Micros, withSubSecond]) {
        final input = DateTime.fromMicrosecondsSinceEpoch(micros, isUtc: true);
        final output = echoTimeSync(value: input);
        expect(output.microsecondsSinceEpoch, micros);
        expect(output.isUtc, isTrue);
        expect(output, input);
      }
    });

    test('microsecond precision is preserved (not truncated to millis)', () async {
      final input = DateTime.fromMicrosecondsSinceEpoch(withSubSecond, isUtc: true);
      final output = await echoTime(value: input);
      // Last three digits (456) would be lost at millisecond precision.
      expect(output.microsecond, 456);
      expect(output.millisecond, 123);
    });

    test('result carries no offset (UTC, isUtc == true)', () async {
      final input = DateTime.fromMicrosecondsSinceEpoch(y2020Micros, isUtc: true);
      final output = await echoTime(value: input);
      expect(output.isUtc, isTrue);
      expect(output.timeZoneOffset, Duration.zero);
      // Dart's toIso8601String renders millisecond precision (3 fractional digits).
      expect(output.toIso8601String(), '2020-01-01T00:00:00.000Z');
    });
  });
}
