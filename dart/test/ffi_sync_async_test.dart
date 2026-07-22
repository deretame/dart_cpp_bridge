import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';
import 'package:test/test.dart';

import 'support/library_path.dart';

void main() {
  late DartCppBridge bridge;

  setUpAll(() async {
    final lib = resolveNativeLibraryPath();
    // ignore: avoid_print
    print('Loading native library: $lib');
    bridge = await DartCppBridge.init(libraryPath: lib);
  });

  tearDownAll(() {
    // setUpAll may have failed before bridge was created.
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

    test('sleepTest (normal / spawn_blocking) returns Done', () async {
      final sw = Stopwatch()..start();
      final out = await bridge.sleepTest();
      sw.stop();
      expect(out, 'Done');
      // C++ sleeps ~1s; allow CI jitter.
      expect(sw.elapsedMilliseconds, greaterThanOrEqualTo(800));
    }, timeout: const Timeout(Duration(seconds: 10)));
  });
}
