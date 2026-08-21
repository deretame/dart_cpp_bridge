import 'package:codegen_demo/codegen_demo.dart';
import 'package:flutter/material.dart';

void main() {
  runApp(const MyApp());
}

class MyApp extends StatefulWidget {
  const MyApp({super.key});

  @override
  State<MyApp> createState() => _MyAppState();
}

class _MyAppState extends State<MyApp> {
  final _results = <String>[];
  var _initialized = false;

  Future<void> _runTests() async {
    final results = <String>[];
    try {
      if (!_initialized) {
        await DcbLib.init();
        _initialized = true;
        results.add('init: OK');
      }

      // Sync
      final ver = bridgeVersion();
      results.add('bridgeVersion: $ver ${ver == 42 ? "PASS" : "FAIL"}');

      // Async
      final sum = await add(a: 10, b: 32);
      results.add('add(10,32): $sum ${sum == 42 ? "PASS" : "FAIL"}');

      // Normal (thread pool)
      final greeting = await sleepGreeting(name: 'Android');
      results.add(
        'sleepGreeting: "$greeting" '
        '${greeting == "hello, Android" ? "PASS" : "FAIL"}',
      );

      // Stream
      final ticks = await tickStream(count: 5, intervalMs: 10).toList();
      results.add('tickStream: $ticks ${ticks.length == 5 ? "PASS" : "FAIL"}');

      // DartFn callback
      final fnResult = await greetDartFn(
        callback: (name) async => 'Dart $name',
        name: 'device',
      );
      results.add(
        'greetDartFn: "$fnResult" '
        '${fnResult == "hello, Dart device" ? "PASS" : "FAIL"}',
      );

      // A DartFn call that waits synchronously in native code must run on the
      // native pool; otherwise the Dart isolate cannot service the callback.
      final blockingUs = await syncDartFnBlockingUs(
        callback: (input) async => 'echo:$input',
        input: 'device',
      );
      results.add(
        'syncDartFnBlockingUs: $blockingUs '
        '${blockingUs >= 0 ? "PASS" : "FAIL"}',
      );

      // BRIDGE_PERSIST keeps the latest callback for this registration key;
      // registering again must replace the old Dart closure rather than grow
      // the per-session callback map without bound.
      final registered1 = registerDartFn(
        callback: (input) async => 'first:$input',
      );
      final registered1Result = await invokeRegistered(input: 'one');
      final registered2 = registerDartFn(
        callback: (input) async => 'second:$input',
      );
      final registered2Result = await invokeRegistered(input: 'two');
      results.add(
        'persistentDartFn: $registered1/$registered2 '
        '${registered1Result == "registered:first:one" && registered2Result == "registered:second:two" ? "PASS" : "FAIL"}',
      );

      // Opaque class
      final counter = Counter.int32T(initialValue: 100);
      await counter.increment(delta: 23);
      final val = await counter.value();
      results.add('Counter: $val ${val == 123 ? "PASS" : "FAIL"}');
      counter.dispose();

      // Data class
      final dist = await distance(
        a: const Point(x: 0, y: 0),
        b: const Point(x: 3, y: 4),
      );
      results.add(
        'distance: $dist ${(dist - 5.0).abs() < 1e-9 ? "PASS" : "FAIL"}',
      );
    } catch (e, st) {
      results.add('ERROR: $e\n$st');
    }
    setState(() => _results.addAll(results));
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        appBar: AppBar(title: const Text('dart_cpp_bridge Android')),
        body: Column(
          children: [
            ElevatedButton(
              onPressed: _runTests,
              child: const Text('Run Bridge Tests'),
            ),
            Expanded(
              child: ListView(
                children: _results
                    .map(
                      (r) => Padding(
                        padding: const EdgeInsets.symmetric(
                          horizontal: 16,
                          vertical: 4,
                        ),
                        child: Text(
                          r,
                          style: TextStyle(
                            color: r.contains('FAIL') || r.contains('ERROR')
                                ? Colors.red
                                : Colors.green,
                            fontFamily: 'monospace',
                            fontSize: 13,
                          ),
                        ),
                      ),
                    )
                    .toList(),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
