import 'dart:io';

import 'package:codegen_demo/codegen_demo.dart';
import 'package:test/test.dart';

String resolveDemoLibrary() {
  const fromDefine = String.fromEnvironment('DCB_LIBRARY_PATH');
  if (fromDefine.isNotEmpty) return fromDefine;
  final fromEnv = Platform.environment['DCB_LIBRARY_PATH'];
  if (fromEnv != null && fromEnv.isNotEmpty) return fromEnv;

  final roots = [
    Directory.current,
    Directory.current.parent,
    Directory.current.parent.parent,
  ];
  final names = [
    if (Platform.isWindows) ...[
      'build/Release/dcb_codegen_demo.dll',
      'build/Debug/dcb_codegen_demo.dll',
      'build/dcb_codegen_demo.dll',
    ],
    if (Platform.isLinux) 'build/libdcb_codegen_demo.so',
    if (Platform.isMacOS) 'build/libdcb_codegen_demo.dylib',
  ];
  for (final root in roots) {
    for (final rel in names) {
      final f = File(
        '${root.path}${Platform.pathSeparator}${rel.replaceAll('/', Platform.pathSeparator)}',
      );
      if (f.existsSync()) return f.path;
    }
  }
  throw StateError(
    'dcb_codegen_demo library not found. Build examples/codegen_demo first.',
  );
}

void main() {
  setUpAll(() async {
    await initBridge(libraryPath: resolveDemoLibrary());
  });

  tearDownAll(() {
    shutdownBridge();
  });

  test('BRIDGE_SYNC bridge_version', () {
    expect(bridgeVersion(), 42);
  });

  test('BRIDGE_ASYNC add', () async {
    expect(await add(2, 3), 5);
  });

  test('BRIDGE_NORMAL sleep_greeting', () async {
    expect(await sleepGreeting('Ada'), 'hello, Ada');
  });

  test('BRIDGE_ASYNC enum next_status', () async {
    expect(await nextStatus(OrderStatus.created), OrderStatus.paid);
    expect(await nextStatus(OrderStatus.paid), OrderStatus.shipped);
    expect(await nextStatus(OrderStatus.shipped), OrderStatus.created);
  });

  test('BRIDGE_ASYNC optional maybe_double', () async {
    expect(await maybeDouble(null), isNull);
    expect(await maybeDouble(5), 10);
    expect(await maybeDouble(-3), -6);
  });

  test('BRIDGE_ASYNC u32 increment_u32', () async {
    expect(await incrementU32(0), 1);
    expect(await incrementU32(4294967290), 4294967291);
  });

  test('BRIDGE_ASYNC i64 increment_i64', () async {
    expect(await incrementI64(0), 1);
    expect(await incrementI64(9223372036854775800), 9223372036854775801);
    expect(await incrementI64(-9223372036854775800), -9223372036854775799);
  });

  test('BRIDGE_ASYNC bool negate_bool', () async {
    expect(await negateBool(true), false);
    expect(await negateBool(false), true);
  });

  test('BRIDGE_ASYNC optional string', () async {
    expect(await optionalString(null), isNull);
    expect(await optionalString('hello'), 'hello!');
  });

  test('BRIDGE_ASYNC optional enum', () async {
    expect(await optionalStatus(null), isNull);
    expect(await optionalStatus(OrderStatus.created), OrderStatus.paid);
    expect(await optionalStatus(OrderStatus.shipped), OrderStatus.created);
  });

  test('BRIDGE_ASYNC vector<int> echo_list', () async {
    expect(await echoList([]), <int>[]);
    expect(await echoList([1, 2, 3]), [1, 2, 3]);
    expect(await echoList([-1, 0, 42]), [-1, 0, 42]);
  });

  test('BRIDGE_ASYNC array<int, 4> sum_array', () async {
    expect(await sumArray([1, 2, 3, 4]), 10);
    expect(await sumArray([-1, 1, -1, 1]), 0);
  });

  test('BRIDGE_ASYNC map<string, int> sum_scores', () async {
    expect(await sumScores({}), 0);
    expect(await sumScores({'a': 1, 'b': 2, 'c': 3}), 6);
  });

  test('BRIDGE_ASYNC set<int> sum_set', () async {
    expect(await sumSet(<int>{}), 0);
    expect(await sumSet({1, 2, 3}), 6);
  });

  test('BRIDGE_ASYNC Int128 echo_i128', () async {
    final big = BigInt.parse('170141183460469231731687303715884105727');
    expect(await echoI128(big), big);
    expect(await echoI128(BigInt.zero), BigInt.zero);
    expect(
      await echoI128(BigInt.parse('-170141183460469231731687303715884105728')),
      BigInt.parse('-170141183460469231731687303715884105728'),
    );
  });

  test('BRIDGE_ASYNC UInt128 echo_u128', () async {
    final big = BigInt.parse('340282366920938463463374607431768211455');
    expect(await echoU128(big), big);
    expect(await echoU128(BigInt.zero), BigInt.zero);
  });

  test('BRIDGE_ASYNC DartFn greet_dart_fn', () async {
    expect(
      await greetDartFn((name) => 'Dart $name', 'world'),
      'hello, Dart world',
    );
    expect(
      await greetDartFn((name) async {
        await Future<void>.delayed(const Duration(milliseconds: 10));
        return 'async $name';
      }, 'moon'),
      'hello, async moon',
    );
  });

  test('BRIDGE_ASYNC pair<int, string> pair_echo', () async {
    expect(await pairEcho((1, 'hello')), (1, 'hello'));
    expect(await pairEcho((-42, 'world')), (-42, 'world'));
  });

  test('BRIDGE_ASYNC tuple<int, string, bool> tuple_echo', () async {
    expect(await tupleEcho((1, 'hello', true)), (1, 'hello', true));
    expect(await tupleEcho((-42, 'world', false)), (-42, 'world', false));
  });

  test('Stream tick_stream emits 0..count-1 then done', () async {
    final values = await tickStream(5, 10).toList();
    expect(values, [0, 1, 2, 3, 4]);
  });

  test('Stream tick_stream cancels subscription', () async {
    final stream = tickStream(100, 10);
    final sub = stream.listen(null);
    await Future<void>.delayed(const Duration(milliseconds: 30));
    await sub.cancel();
  });

  test('data class distance', () async {
    final a = const Point(x: 0.0, y: 0.0);
    final b = const Point(x: 3.0, y: 4.0);
    expect(await distance(a, b), closeTo(5.0, 1e-9));
    expect(await distance(a, a), closeTo(0.0, 1e-9));
  });

  test('data class scale', () async {
    final p = const Point(x: 1.5, y: -2.0);
    final scaled = await scale(p, 2.0);
    expect(scaled.x, closeTo(3.0, 1e-9));
    expect(scaled.y, closeTo(-4.0, 1e-9));
  });

  test('data class bounding_box', () async {
    final points = [
      const Point(x: 1.0, y: 2.0),
      const Point(x: -3.0, y: 4.0),
      const Point(x: 0.0, y: -1.0),
    ];
    final box = await boundingBox(points);
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
    final counter = Counter.withInitialValue(initialValue: 10);
    expect(await counter.value(), 10);
    expect(counter.valueSync(), 10);
  });

  test('opaque class Counter default constructor', () async {
    final counter = Counter();
    expect(await counter.value(), 0);
  });

  test('opaque class Counter increment and default delta', () async {
    final counter = Counter.withInitialValue(initialValue: 5);
    await counter.increment();
    expect(await counter.value(), 6);
    await counter.increment(3);
    expect(await counter.value(), 9);
  });

  test('opaque class Counter addList', () async {
    final counter = Counter.withInitialValue(initialValue: 10);
    expect(await counter.addList([1, 2, 3]), 16);
    expect(await counter.value(), 16);
  });

  test('opaque class Counter setValue', () async {
    final counter = Counter.withInitialValue(initialValue: 0);
    await counter.setValue(42);
    expect(await counter.value(), 42);
    await counter.setValue(null);
    expect(await counter.value(), 42);
  });

  test('opaque class Counter duplicate', () async {
    final counter = Counter.withInitialValue(initialValue: 7);
    final copy = await counter.duplicate();
    expect(await copy.value(), 7);
    await counter.increment();
    expect(await counter.value(), 8);
    expect(await copy.value(), 7);
  });

  test('opaque class Counter static sum', () {
    expect(Counter.sum(3, 4), 7);
  });

  test('opaque class Counter sleepAndGet normal method', () async {
    final counter = Counter.withInitialValue(initialValue: 100);
    expect(await counter.sleepAndGet(50), 100);
  });

  test('opaque class Counter greetDartFn', () async {
    final counter = Counter.withInitialValue(initialValue: 5);
    final result = await counter.greetDartFn(
      (value) => 'Dart got $value',
      'world',
    );
    expect(result, 'hello, Dart got world');
  });

  test('opaque class Counter tickStream', () async {
    final counter = Counter.withInitialValue(initialValue: 3);
    final values = await counter.tickStream(3, 10).toList();
    expect(values, [3, 3, 3]);
  });

  test('opaque class Counter dispose then throws', () async {
    final counter = Counter.withInitialValue(initialValue: 1);
    counter.dispose();
    expect(() => counter.valueSync(), throwsA(isA<StateError>()));
  });

  test('opaque class Counter instances are independent', () async {
    final a = Counter.withInitialValue(initialValue: 1);
    final b = Counter.withInitialValue(initialValue: 2);
    await a.increment();
    expect(await a.value(), 2);
    expect(await b.value(), 2);
    await b.increment(5);
    expect(await a.value(), 2);
    expect(await b.value(), 7);
  });

  test('opaque class Counter destructor is called on dispose', () async {
    // Record baseline alive count (other tests may have leaked counters).
    final baseline = Counter.aliveCount();

    final c1 = Counter.withInitialValue(initialValue: 100);
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
    final c = Counter.withInitialValue(initialValue: 1);
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
      final original = Counter.withInitialValue(initialValue: 42);
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

  // --- Runtime error propagation tests (R01-R06) ---

  group('runtime error propagation', () {
    test('R01: async throw surfaces as StateError', () async {
      await expectLater(
        failAsync('boom-async'),
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
        () => failSync('boom-sync'),
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
        failNormal('boom-normal'),
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
        await for (final v in failStream('boom-stream')) {
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
      await expectLater(failAsync('temp-error'), throwsA(isA<StateError>()));
      // Session should still work normally.
      expect(await add(10, 20), 30);
      expect(bridgeVersion(), 42);
    });
  });

  // --- Deep nesting test (G03) ---

  group('deep nesting containers', () {
    test('G03: 3-level nested vector roundtrip', () {
      final cube = nestedCube(2);
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
      final cube = nestedCube(0);
      expect(cube, isEmpty);
    });
  });
}
