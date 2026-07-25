import 'dart:io';

/// Resolve native library path for base_demo tests.
///
/// Order:
/// 1. env `DCB_LIBRARY_PATH` or `--dart-define=DCB_LIBRARY_PATH=...`
/// 2. Common build outputs relative to `examples/base_demo/`
String resolveNativeLibraryPath() {
  const fromDefine = String.fromEnvironment('DCB_LIBRARY_PATH');
  if (fromDefine.isNotEmpty) {
    return fromDefine;
  }

  final fromEnv = Platform.environment['DCB_LIBRARY_PATH'];
  if (fromEnv != null && fromEnv.isNotEmpty) {
    return fromEnv;
  }

  // `dart test` from examples/base_demo/ => cwd is `.../examples/base_demo`
  final cwd = Directory.current;
  final roots = <Directory>[
    cwd,
    cwd.parent,
    cwd.parent.parent,
  ];

  final names = <String>[
    if (Platform.isWindows) ...[
      'build/Release/dart_cpp_bridge.dll',
      'build/Debug/dart_cpp_bridge.dll',
      'examples/base_demo/build/Release/dart_cpp_bridge.dll',
      'examples/base_demo/build/Debug/dart_cpp_bridge.dll',
    ],
    if (Platform.isLinux) ...[
      'build/libdart_cpp_bridge.so',
      'build/Release/libdart_cpp_bridge.so',
      'examples/base_demo/build/libdart_cpp_bridge.so',
    ],
    if (Platform.isMacOS) ...[
      'build/libdart_cpp_bridge.dylib',
      'build/Release/libdart_cpp_bridge.dylib',
      'examples/base_demo/build/libdart_cpp_bridge.dylib',
    ],
  ];

  final tried = <String>[];
  for (final root in roots) {
    for (final rel in names) {
      final f = File('${root.path}${Platform.pathSeparator}${rel.replaceAll('/', Platform.pathSeparator)}');
      tried.add(f.path);
      if (f.existsSync()) {
        return f.path;
      }
    }
  }

  throw StateError(
    'Native library not found. Build examples/base_demo first, or set DCB_LIBRARY_PATH.\n'
    'cwd=${cwd.path}\n'
    'Tried:\n${tried.map((e) => '  - $e').join('\n')}',
  );
}
