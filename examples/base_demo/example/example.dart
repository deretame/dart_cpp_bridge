// ignore_for_file: avoid_print

/// Minimal usage sketch for [package:dart_cpp_bridge].
///
/// Run with `dart test` (hooks build the native library automatically).
///
/// Full docs: https://github.com/deretame/dart_cpp_bridge
library;

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';
import 'package:dcb_base_demo/demo_bridge.dart';
import 'package:dcb_base_demo/src/dcb_bindings.dart';

Future<void> main(List<String> args) async {
  final bridge = await DartCppBridge.init(bindings: createDcbBindings());

  print('bridgeVersion = ${bridge.bridgeVersion()}');
  print('add(40, 2)    = ${await bridge.add(40, 2)}');
  print('echo          = ${await bridge.echo('hi')}');

  await for (final n in bridge.ticks(count: 3, intervalMs: 0)) {
    print('tick $n');
  }

  // Main isolate only — stops the process-wide C++ runtime.
  bridge.shutdown();
  print('done');
}
