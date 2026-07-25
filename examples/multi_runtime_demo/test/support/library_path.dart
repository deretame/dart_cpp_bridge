import 'dart:io';

/// Resolve the path to the multi_runtime_demo native DLL.
String resolveLibraryPath() {
  final env = Platform.environment['DCB_LIBRARY_PATH'];
  if (env != null && env.isNotEmpty) return env;

  // Walk up from test/ to project root, then into build/Release.
  final candidates = [
    'build/Release/dart_cpp_bridge.dll',
    'build/libdart_cpp_bridge.so',
    'build/libdart_cpp_bridge.dylib',
  ];
  for (final c in candidates) {
    if (File(c).existsSync()) return File(c).absolute.path;
  }
  throw StateError(
    'Native library not found. Build first:\n'
    '  cmake -S . -B build && cmake --build build --config Release\n'
    'Or set DCB_LIBRARY_PATH.',
  );
}
