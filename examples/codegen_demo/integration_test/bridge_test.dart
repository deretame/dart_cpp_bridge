import 'package:codegen_demo/codegen_demo.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  setUpAll(() async {
    await DcbLib.init();
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
      callback: (name) async => 'Dart $name',
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
