import 'dart:io';

import 'package:codegen_demo/codegen_demo.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    // On Android, the combined library (runtime + dispatch) is
    // libdcb_codegen_demo.so, extracted to the app's native lib dir.
    // DynamicLibrary.open with just the soname works because Android
    // includes the app's lib directory in the linker search path.
    final libPath = Platform.isAndroid ? 'libdcb_codegen_demo.so' : null;
    await DcbLib.init(libraryPath: libPath);
  });

  tearDownAll(() {
    DcbLib.shutdown();
  });

  testWidgets('bridge sync/async/normal/stream/dartfn/opaque', (tester) async {
    // Sync
    expect(bridgeVersion(), 42);

    // Async
    expect(await add(a: 10, b: 32), 42);

    // Normal (thread pool)
    expect(await sleepGreeting(name: 'Android'), 'hello, Android');

    // Stream
    final ticks = await tickStream(count: 5, intervalMs: 10).toList();
    expect(ticks, [0, 1, 2, 3, 4]);

    // DartFn reverse callback
    final fnResult = await greetDartFn(
      callback: (name) => 'Dart $name',
      name: 'device',
    );
    expect(fnResult, 'hello, Dart device');

    // Opaque class
    final counter = Counter.int32T(initialValue: 100);
    await counter.increment(delta: 23);
    expect(await counter.value(), 123);
    expect(counter.toString(), 'Counter(value: 123)');
    counter.dispose();

    // Data class
    final dist = await distance(
      a: const Point(x: 0, y: 0),
      b: const Point(x: 3, y: 4),
    );
    expect(dist, closeTo(5.0, 1e-9));

    // Error propagation
    await expectLater(
      failAsync(msg: 'android-boom'),
      throwsA(isA<StateError>()),
    );

    // Session recovery after error
    expect(await add(a: 1, b: 2), 3);
  });
}
