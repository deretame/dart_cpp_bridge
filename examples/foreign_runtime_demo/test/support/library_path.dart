import 'dart:io';

/// Locate the native library for tests.
///
/// Priority:
/// 1. DCB_LIBRARY_PATH environment variable
/// 2. Relative to current working directory (dart test runs from project root)
String get libraryPath {
  final env = Platform.environment['DCB_LIBRARY_PATH'];
  if (env != null && env.isNotEmpty) return env;

  // dart test 从项目根目录运行
  if (Platform.isWindows) {
    return 'build/Release/dart_cpp_bridge.dll';
  } else if (Platform.isMacOS) {
    return 'build/libdart_cpp_bridge.dylib';
  } else {
    return 'build/libdart_cpp_bridge.so';
  }
}
