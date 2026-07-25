import 'dart:io';

import 'package:path/path.dart' as p;

import 'package:dcb_codegen/src/bootstrap.dart';
import 'package:dcb_codegen/src/lock_file.dart';
import 'package:dcb_codegen/src/platform.dart';

const _usage = '''
dart_cpp_bridge codegen tool (Dart)

Usage:
  dart run bin/codegen.dart [options] [script.py [args...]]

Options:
  --force    Force re-download / re-extract even if the cached toolchain
             appears valid.
  --help     Show this message.

If no script is given, only bootstrap (download + smoke-test) is performed.

Examples:
  # Bootstrap only
  dart run bin/codegen.dart

  # Run the full codegen pipeline
  dart run bin/codegen.dart scripts/run_codegen.py ../examples/codegen_demo/dart_cpp_bridge.yaml

  # Run tests
  dart run bin/codegen.dart tests/run_tests.py

  # Force rebuild
  dart run bin/codegen.dart --force scripts/run_codegen.py config.yaml
''';

Future<void> main(List<String> arguments) async {
  // --- Parse arguments ---------------------------------------------------
  var force = false;
  final rest = <String>[];

  for (final arg in arguments) {
    if (arg == '--help' || arg == '-h') {
      stdout.write(_usage);
      exit(0);
    } else if (arg == '--force') {
      force = true;
    } else {
      rest.add(arg);
    }
  }

  // --- Detect platform & parse lock file ---------------------------------
  final platform = HostPlatform.detect();
  final codegenRoot = _findCodegenRoot();
  final lockFile = File(p.join(codegenRoot, 'versions.lock'));
  if (!lockFile.existsSync()) {
    stderr.writeln(
        'error: versions.lock not found at ${lockFile.path}.\n'
        'Run this tool from the codegen/ directory or ensure the file exists.');
    exit(1);
  }

  final lock = LockFile.parse(lockFile);
  final cacheRoot = platform.resolveCacheRoot();

  // --- Bootstrap ---------------------------------------------------------
  final BootstrapResult result;
  try {
    result = await bootstrap(
      platform,
      lock,
      cacheRoot: cacheRoot,
      options: BootstrapOptions(force: force),
    );
  } catch (e) {
    stderr.writeln('error: bootstrap failed: $e');
    exit(1);
  }

  // --- Run script (if provided) ------------------------------------------
  if (rest.isEmpty) {
    stderr.writeln('[codegen] Bootstrap complete. No script specified.');
    exit(0);
  }

  final script = rest.first;
  final scriptArgs = rest.skip(1).toList();

  // Resolve script path relative to codegen root.
  final scriptPath =
      p.isAbsolute(script) ? script : p.join(codegenRoot, script);
  if (!File(scriptPath).existsSync()) {
    stderr.writeln('error: script not found: $scriptPath');
    exit(1);
  }

  stderr.writeln('[codegen] Running: $scriptPath ${scriptArgs.join(' ')}');

  final process = await Process.start(
    result.pythonExe,
    [scriptPath, ...scriptArgs],
    mode: ProcessStartMode.inheritStdio,
    workingDirectory: codegenRoot,
    environment: {
      'DCB_CODEGEN_ENV_KEY': result.envKey,
    },
  );

  final exitCode = await process.exitCode;
  exit(exitCode);
}

/// Locate the codegen/ directory (where versions.lock lives).
///
/// Strategy: walk up from the script's own location (bin/) to find
/// versions.lock. Falls back to CWD.
String _findCodegenRoot() {
  // When run via `dart run bin/codegen.dart`, the script is in codegen/bin/.
  final scriptDir = p.dirname(Platform.script.toFilePath());
  final candidate = p.dirname(scriptDir); // codegen/
  if (File(p.join(candidate, 'versions.lock')).existsSync()) {
    return candidate;
  }

  // Fallback: current working directory.
  final cwd = Directory.current.path;
  if (File(p.join(cwd, 'versions.lock')).existsSync()) {
    return cwd;
  }

  // Last resort: return cwd and let the caller report the missing file.
  return cwd;
}
