import 'package:codegen_demo/codegen_demo.dart';

void main() async {
  await initBridge();

  // Warm up
  syncDartFnBlockingUs(callback: (s) => 'warm', input: 'x');

  // Measure: simple sync callback
  final times = <int>[];
  for (var i = 0; i < 10; i++) {
    final us = syncDartFnBlockingUs(callback: (s) => 'ok: $s', input: 'test$i');
    times.add(us);
  }
  times.sort();
  print('callSync on io thread (BRIDGE_SYNC):');
  print('  min: ${times.first} µs');
  print('  median: ${times[times.length ~/ 2]} µs');
  print('  max: ${times.last} µs');
  print('  all: $times');

  BridgeApiImpl.instance.dispose();
}
