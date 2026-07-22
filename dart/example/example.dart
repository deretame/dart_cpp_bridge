// ignore_for_file: avoid_print

/// Minimal usage sketch for [package:dart_cpp_bridge].
///
/// Build the native library from the monorepo first, then:
///
/// ```bash
/// cd dart
/// dart run example/example.dart
/// # or: DCB_LIBRARY_PATH=/path/to/lib dart run example/example.dart
/// ```
///
/// Full docs: https://github.com/deretame/dart_cpp_bridge
library;

import 'dart:io';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

Future<void> main(List<String> args) async {
  final path = Platform.environment['DCB_LIBRARY_PATH'] ??
      (args.isNotEmpty ? args.first : _defaultLibraryPath());

  print('Loading: $path');
  final bridge = await DartCppBridge.init(libraryPath: path);

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

String _defaultLibraryPath() {
  if (Platform.isWindows) {
    return '../build/Release/dart_cpp_bridge.dll';
  }
  if (Platform.isMacOS) {
    return '../build/libdart_cpp_bridge.dylib';
  }
  return '../build/libdart_cpp_bridge.so';
}
