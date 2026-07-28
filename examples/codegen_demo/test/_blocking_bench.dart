import 'package:codegen_demo/codegen_demo.dart';

void main() async {
  await initBridge();

  // Warm up
  syncDartFnBlockingUs(callback: (s) async => 'warm', input: 'x');

  // Measure: simple sync callback
  final times = <int>[];
  for (var i = 0; i < 10; i++) {
    final us = syncDartFnBlockingUs(callback: (s) async => 'ok: $s', input: 'test$i');
    times.add(us);
  }
  times.sort();
  print('syncAwait on pool thread (BRIDGE_SYNC):');
  print('  min: ${times.first} µs');
  print('  median: ${times[times.length ~/ 2]} µs');
  print('  max: ${times.last} µs');
  print('  all: $times');

  BridgeApiImpl.instance.dispose();
}
