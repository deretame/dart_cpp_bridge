import 'dart:io';
import 'dart:isolate';

import 'package:path/path.dart' as p;

/// Resolve the absolute path to the dcb_gen_tool package root directory.
///
/// Works in all execution modes:
///   - `dart run bin/dcb_gen.dart` (development)
///   - `dart pub global activate dcb_gen_tool` (global activation)
///   - `dart pub global activate --source path ./dcb_gen_tool` (path override)
///
/// Strategy:
///   1. Use `Isolate.resolvePackageUri` to locate `package:dcb_gen_tool/`
///      which resolves to `<pkg>/lib/`, then go up one level.
///   2. Fallback: walk up from `Platform.script` looking for `versions.lock`.
///   3. Fallback: current working directory.
Future<String> resolvePackageRoot() async {
  // Strategy 1: package URI resolution (works for global activation).
  try {
    final libUri = await Isolate.resolvePackageUri(
        Uri.parse('package:dcb_gen_tool/'));
    if (libUri != null && libUri.scheme == 'file') {
      final libDir = libUri.toFilePath();
      final root = p.dirname(libDir); // lib/ → package root
      if (File(p.join(root, 'versions.lock')).existsSync()) {
        return root;
      }
    }
  } catch (_) {
    // Fall through to next strategy.
  }

  // Strategy 2: walk up from Platform.script.
  try {
    var dir = p.dirname(Platform.script.toFilePath());
    for (var i = 0; i < 5; i++) {
      if (File(p.join(dir, 'versions.lock')).existsSync() &&
          Directory(p.join(dir, 'scripts')).existsSync()) {
        return dir;
      }
      final parent = p.dirname(dir);
      if (parent == dir) break;
      dir = parent;
    }
  } catch (_) {
    // Fall through.
  }

  // Strategy 3: CWD (last resort).
  final cwd = Directory.current.path;
  if (File(p.join(cwd, 'versions.lock')).existsSync()) {
    return cwd;
  }

  throw StateError(
      'Cannot locate dcb_gen_tool package root (versions.lock not found). '
      'Ensure the package is properly installed or run from the package directory.');
}
