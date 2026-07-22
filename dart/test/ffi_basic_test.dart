import 'dart:async';
import 'dart:isolate';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';
import 'package:test/test.dart';

import 'support/library_path.dart';

/// Top-level entry for worker isolates (must not capture main bridge).
/// No manual dispose — NativeFinalizer closes the session when the isolate ends.
Future<int> _workerAdd(String libraryPath, int a, int b) {
  return Isolate.run(() async {
    final bridge = await DartCppBridge.init(libraryPath: libraryPath);
    return bridge.add(a, b);
  });
}

Future<List<int>> _workerTicks(String libraryPath) {
  return Isolate.run(() async {
    final bridge = await DartCppBridge.init(libraryPath: libraryPath);
    return bridge.ticks(count: 3, intervalMs: 5).toList();
  });
}

void main() {
  late String libraryPath;
  late DartCppBridge bridge;

  Future<DartCppBridge> openBridge() => DartCppBridge.init(libraryPath: libraryPath);

  setUpAll(() async {
    libraryPath = resolveNativeLibraryPath();
    // ignore: avoid_print
    print('Loading native library: $libraryPath');
    bridge = await openBridge();
  });

  tearDownAll(() {
    try {
      bridge.shutdown();
    } catch (_) {}
  });

  group('sync FFI', () {
    test('bridgeVersion returns 1', () {
      expect(bridge.bridgeVersion(), 1);
    });

    test('bridgeVersion is stable across calls', () {
      expect(bridge.bridgeVersion(), bridge.bridgeVersion());
    });
  });

  group('async FFI', () {
    test('add returns sum via Future', () async {
      await expectLater(bridge.add(40, 2), completion(42));
    });

    test('add handles negatives', () async {
      expect(await bridge.add(-3, 10), 7);
    });

    test('concurrent add calls complete correctly', () async {
      final results = await Future.wait([
        bridge.add(1, 2),
        bridge.add(10, 20),
        bridge.add(100, 200),
      ]);
      expect(results, [3, 30, 300]);
    });

    test('sleepTest (normal / thread_pool) returns Done', () async {
      final sw = Stopwatch()..start();
      final out = await bridge.sleepTest();
      sw.stop();
      expect(out, 'Done');
      expect(sw.elapsedMilliseconds, greaterThanOrEqualTo(800));
    }, timeout: const Timeout(Duration(seconds: 10)));
  });

  group('stream FFI', () {
    test('ticks emits 0..count-1 then done', () async {
      final values = await bridge.ticks(count: 5, intervalMs: 20).toList();
      expect(values, [0, 1, 2, 3, 4]);
    });

    test('ticks with zero interval still completes', () async {
      final values = await bridge.ticks(count: 3, intervalMs: 0).toList();
      expect(values, [0, 1, 2]);
    });

    test('cancel subscription stops delivering', () async {
      final received = <int>[];
      final sub = bridge.ticks(count: 30, intervalMs: 40).listen(received.add);
      await Future<void>.delayed(const Duration(milliseconds: 80));
      expect(received, isNotEmpty);
      final atCancel = received.length;
      await sub.cancel();
      await Future<void>.delayed(const Duration(milliseconds: 300));
      expect(received.length, atCancel);
    });

    test('two concurrent ticks streams are independent', () async {
      final results = await Future.wait([
        bridge.ticks(count: 3, intervalMs: 10).toList(),
        bridge.ticks(count: 4, intervalMs: 10).toList(),
      ]);
      expect(results[0], [0, 1, 2]);
      expect(results[1], [0, 1, 2, 3]);
    });
  });

  group('errors & payload', () {
    test('echo roundtrips utf-8 string', () async {
      const s = '你好 dart_cpp_bridge ✓';
      expect(await bridge.echo(s), s);
    });

    test('echo large payload (~256KiB)', () async {
      final s = 'x' * (256 * 1024);
      final out = await bridge.echo(s);
      expect(out.length, s.length);
    });

    test('failAsync surfaces as Future error', () async {
      await expectLater(
        bridge.failAsync('boom-async'),
        throwsA(isA<StateError>().having((e) => e.message, 'message', contains('boom-async'))),
      );
    });

    test('failStream emits data then error', () async {
      final values = <int>[];
      Object? err;
      try {
        await for (final v in bridge.failStream('boom-stream')) {
          values.add(v);
        }
      } catch (e) {
        err = e;
      }
      expect(values, [1]);
      expect(err, isA<StateError>());
      expect((err! as StateError).message, contains('boom-stream'));
    });

    test('unknown async method errors', () async {
      await expectLater(
        bridge.invokeUnknownMethodForTest(),
        throwsA(isA<StateError>().having((e) => e.message, 'message', contains('unknown method'))),
      );
    });

    test('sync non-sync method errors', () {
      expect(
        () => bridge.invokeSyncNonSyncMethodForTest(),
        throwsA(isA<StateError>().having((e) => e.message, 'message', contains('not sync-capable'))),
      );
    });

    test('bad frame from FFI surfaces error', () async {
      await expectLater(
        bridge.invokeBadFrameForTest(),
        throwsA(isA<StateError>().having((e) => e.message, 'message', contains('bad frame'))),
      );
    });
  });

  group('multi isolate (per-isolate session, shared runtime)', () {
    test('background isolates can async add', () async {
      final results = await Future.wait([
        _workerAdd(libraryPath, 1, 2),
        _workerAdd(libraryPath, 10, 20),
        _workerAdd(libraryPath, 100, 200),
      ]);
      expect(results, [3, 30, 300]);
      // Main isolate session still works.
      expect(await bridge.add(7, 8), 15);
    });

    test('background isolate can stream ticks', () async {
      expect(await _workerTicks(libraryPath), [0, 1, 2]);
    });

    test('many concurrent worker async calls', () async {
      final futures = <Future<int>>[];
      for (var i = 0; i < 12; i++) {
        futures.add(_workerAdd(libraryPath, i, i));
      }
      final results = await Future.wait(futures);
      for (var i = 0; i < 12; i++) {
        expect(results[i], i + i);
      }
    });
  });

  group('lifecycle', () {
    test('dispose completes pending Future with error', () async {
      final pending = bridge.sleepTest();
      await Future<void>.delayed(const Duration(milliseconds: 50));
      bridge.dispose();
      await expectLater(
        pending,
        throwsA(isA<StateError>().having((e) => e.message, 'message', contains('disposed'))),
      );
      bridge = await openBridge();
      expect(bridge.bridgeVersion(), 1);
    }, timeout: const Timeout(Duration(seconds: 10)));

    test('works after re-init', () async {
      expect(await bridge.add(1, 1), 2);
      expect(await bridge.ticks(count: 2, intervalMs: 0).toList(), [0, 1]);
    });

    test('shutdown then call fails; can start again', () async {
      bridge.shutdown();
      expect(() => bridge.bridgeVersion(), throwsA(isA<StateError>()));
      bridge = await openBridge();
      expect(bridge.bridgeVersion(), 1);
      expect(await bridge.add(4, 5), 9);
    });
  });
}
