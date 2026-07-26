import 'dart:io';

import 'package:path/path.dart' as p;

import 'bootstrap.dart';
import 'lock_file.dart';
import 'package_root.dart';
import 'platform.dart';

const _version = '0.1.0';

const _usage = '''
dcb_gen — dart_cpp_bridge code generation tool ($_version)

Usage:
  dcb_gen <command> [options]

Commands:
  generate <config.yaml>   Run the full codegen pipeline (parse + generate).
  bootstrap                Download and verify the pinned Python toolchain.
  doctor                   Check environment (Dart SDK, toolchain, CMake).

Options:
  --force                  Force re-download even if cached toolchain is valid.
  --quiet                  Suppress non-error output.
  --version                Print version and exit.
  --help, -h               Show this message.

Examples:
  # First-time setup (downloads ~100 MB toolchain)
  dcb_gen bootstrap

  # Generate bridge code for a project
  dcb_gen generate dart_cpp_bridge.yaml

  # Force re-download toolchain
  dcb_gen bootstrap --force

  # Check environment health
  dcb_gen doctor
''';

/// Main CLI dispatcher. Returns the process exit code.
Future<int> runCli(List<String> arguments) async {
  // --- Global flags ---
  var force = false;
  var quiet = false;
  final positional = <String>[];

  for (final arg in arguments) {
    switch (arg) {
      case '--version':
        stdout.writeln('dcb_gen $_version');
        return 0;
      case '--help':
      case '-h':
        stdout.write(_usage);
        return 0;
      case '--force':
        force = true;
      case '--quiet':
        quiet = true;
      default:
        positional.add(arg);
    }
  }

  if (positional.isEmpty) {
    stdout.write(_usage);
    return 1;
  }

  final command = positional.first;
  final args = positional.skip(1).toList();

  try {
    switch (command) {
      case 'generate':
        return await _cmdGenerate(args, force: force, quiet: quiet);
      case 'bootstrap':
        return await _cmdBootstrap(force: force, quiet: quiet);
      case 'doctor':
        return await _cmdDoctor(quiet: quiet);
      default:
        stderr.writeln('error: unknown command "$command"');
        stderr.writeln('Run "dcb_gen --help" for usage.');
        return 1;
    }
  } catch (e) {
    stderr.writeln('error: $e');
    return 1;
  }
}

// ===========================================================================
// Commands
// ===========================================================================

/// `dcb_gen generate <config.yaml>`
Future<int> _cmdGenerate(
  List<String> args, {
  required bool force,
  required bool quiet,
}) async {
  if (args.isEmpty) {
    stderr.writeln('error: missing <config.yaml> argument.');
    stderr.writeln('Usage: dcb_gen generate <path/to/dart_cpp_bridge.yaml>');
    return 1;
  }

  final configPath = args.first;
  final configFile = File(configPath);
  if (!configFile.existsSync()) {
    stderr.writeln('error: config file not found: $configPath');
    return 1;
  }

  final log = CliLogger(quiet: quiet);
  final packageRoot = await resolvePackageRoot();

  // Bootstrap toolchain.
  final platform = HostPlatform.detect();
  final lockFile = File(p.join(packageRoot, 'versions.lock'));
  if (!lockFile.existsSync()) {
    stderr.writeln('error: versions.lock not found in package: '
        '${lockFile.path}');
    return 1;
  }

  final lock = LockFile.parse(lockFile);
  final cacheRoot = platform.resolveCacheRoot();

  log.info('Bootstrapping toolchain ...');
  final result = await bootstrap(
    platform,
    lock,
    cacheRoot: cacheRoot,
    packageRoot: packageRoot,
    options: BootstrapOptions(force: force, quiet: quiet),
  );

  // Run codegen pipeline.
  final scriptPath = p.join(packageRoot, 'scripts', 'run_codegen.py');
  if (!File(scriptPath).existsSync()) {
    stderr.writeln('error: codegen script not found: $scriptPath');
    return 1;
  }

  final absConfig = p.absolute(configPath);
  log.info('Running codegen: $absConfig');

  final process = await Process.start(
    result.pythonExe,
    [scriptPath, absConfig],
    mode: ProcessStartMode.inheritStdio,
    workingDirectory: p.join(packageRoot, 'scripts'),
    environment: {
      'DCB_CODEGEN_ENV_KEY': result.envKey,
      'DCB_PACKAGE_ROOT': packageRoot,
    },
  );

  return await process.exitCode;
}

/// `dcb_gen bootstrap`
Future<int> _cmdBootstrap({
  required bool force,
  required bool quiet,
}) async {
  final log = CliLogger(quiet: quiet);
  final packageRoot = await resolvePackageRoot();
  final platform = HostPlatform.detect();

  final lockFile = File(p.join(packageRoot, 'versions.lock'));
  if (!lockFile.existsSync()) {
    stderr.writeln('error: versions.lock not found: ${lockFile.path}');
    return 1;
  }

  final lock = LockFile.parse(lockFile);
  final cacheRoot = platform.resolveCacheRoot();

  log.info('Platform: ${platform.key}');
  log.info('Cache root: $cacheRoot');
  log.info('Bootstrapping toolchain ...');

  final result = await bootstrap(
    platform,
    lock,
    cacheRoot: cacheRoot,
    packageRoot: packageRoot,
    options: BootstrapOptions(force: force, quiet: quiet),
  );

  log.info('Done. Python: ${result.pythonExe}');
  return 0;
}

/// `dcb_gen doctor`
Future<int> _cmdDoctor({required bool quiet}) async {
  final log = CliLogger(quiet: false); // doctor always prints
  var ok = true;

  // Dart SDK
  log.info('Dart SDK: ${Platform.version.split(' ').first}');

  // Platform
  final platform = HostPlatform.detect();
  log.info('Platform: ${platform.key}');

  // Package root
  final packageRoot = await resolvePackageRoot();
  log.info('Package root: $packageRoot');

  // versions.lock
  final lockFile = File(p.join(packageRoot, 'versions.lock'));
  if (lockFile.existsSync()) {
    log.info('versions.lock: found');
  } else {
    log.warn('versions.lock: NOT FOUND');
    ok = false;
  }

  // scripts/
  final scriptsDir = Directory(p.join(packageRoot, 'scripts'));
  if (scriptsDir.existsSync()) {
    log.info('scripts/: found');
  } else {
    log.warn('scripts/: NOT FOUND');
    ok = false;
  }

  // Toolchain cache
  final cacheRoot = platform.resolveCacheRoot();
  log.info('Toolchain cache: $cacheRoot');
  final lastEnv = File(p.join(cacheRoot, 'LAST_ENV.json'));
  if (lastEnv.existsSync()) {
    log.info('Cached toolchain: yes');
  } else {
    log.info('Cached toolchain: no (run "dcb_gen bootstrap")');
  }

  // CMake (optional)
  final cmake = _which('cmake');
  if (cmake != null) {
    log.info('CMake: $cmake');
  } else {
    log.info('CMake: not found (needed for building, not for codegen)');
  }

  if (ok) {
    log.info('All checks passed.');
    return 0;
  } else {
    log.warn('Some checks failed.');
    return 1;
  }
}

// ===========================================================================
// Helpers
// ===========================================================================

/// Simple CLI logger that writes to stderr.
class CliLogger {
  final bool quiet;
  CliLogger({required this.quiet});

  void info(String msg) {
    if (!quiet) stderr.writeln('[dcb_gen] $msg');
  }

  void warn(String msg) {
    stderr.writeln('[dcb_gen] WARNING: $msg');
  }
}

/// Find an executable on PATH. Returns full path or null.
String? _which(String exe) {
  try {
    final cmd = Platform.isWindows ? 'where' : 'which';
    final result = Process.runSync(cmd, [exe]);
    if (result.exitCode == 0) {
      final out = (result.stdout as String).trim();
      return out.split('\n').first.trim();
    }
  } catch (_) {}
  return null;
}
