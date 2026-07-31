import 'dart:io';

import 'package:path/path.dart' as p;
import 'package:yaml/yaml.dart';

import 'bootstrap.dart';
import 'init.dart';
import 'lock_file.dart';
import 'package_root.dart';
import 'platform.dart';

final String _version = _readPackageVersion();

String get _usage => '''
dcb_gen_tool — dart_cpp_bridge code generation tool ($_version)

Usage:
  dcb_gen_tool <command> [options]

Commands:
  generate <config.yaml>   Run the full codegen pipeline (parse + generate).
  init [--name <native_lib_name>]
                           Scaffold a new dart_cpp_bridge project.
                           --name sets the native library / CMake target name
                           and defaults to the pubspec.yaml package name.
  bootstrap                Download and verify the pinned Python toolchain.
  doctor                   Check environment (Dart SDK, toolchain, CMake).

Options:
  --force                  Force re-download even if cached toolchain is valid.
  --quiet                  Suppress non-error output.
  --skip-version-check     Skip dart_cpp_bridge version consistency check.
  --version                Print version and exit.
  --help, -h               Show this message.

Examples:
  # First-time setup (downloads ~100 MB toolchain)
  dcb_gen_tool bootstrap

  # Initialize a new bridge project (native lib name defaults to pubspec name)
  dcb_gen_tool init

  # Use a different native library / CMake target name
  dcb_gen_tool init --name my_bridge

  # Generate bridge code for a project
  dcb_gen_tool generate dart_cpp_bridge.yaml

  # Force re-download toolchain
  dcb_gen_tool bootstrap --force

  # Check environment health
  dcb_gen_tool doctor
''';

/// Reads this package's version from `pubspec.yaml`.
///
/// Tries to locate `pubspec.yaml` by walking up from [Platform.script],
/// then falls back to the current working directory. Keeping the version in
/// `pubspec.yaml` avoids having to update a hard-coded constant on every
/// release.
String _readPackageVersion() {
  // Strategy 1: walk up from the running script (works for `dart run`,
  // global activation, and path overrides).
  try {
    var dir = p.dirname(Platform.script.toFilePath());
    for (var i = 0; i < 6; i++) {
      final pubspecFile = File(p.join(dir, 'pubspec.yaml'));
      if (pubspecFile.existsSync()) {
        final content = loadYaml(pubspecFile.readAsStringSync());
        if (content is YamlMap &&
            content['name'] == 'dcb_gen_tool' &&
            content['version'] is String) {
          return content['version'] as String;
        }
      }
      final parent = p.dirname(dir);
      if (parent == dir) break;
      dir = parent;
    }
  } catch (_) {
    // Fall through.
  }

  // Strategy 2: current working directory (last resort).
  try {
    final pubspecFile = File(p.join(Directory.current.path, 'pubspec.yaml'));
    if (pubspecFile.existsSync()) {
      final content = loadYaml(pubspecFile.readAsStringSync());
      if (content is YamlMap &&
          content['name'] == 'dcb_gen_tool' &&
          content['version'] is String) {
        return content['version'] as String;
      }
    }
  } catch (_) {
    // Fall through.
  }

  return 'unknown';
}

/// Main CLI dispatcher. Returns the process exit code.
Future<int> runCli(List<String> arguments) async {
  // --- Global flags ---
  var force = false;
  var quiet = false;
  var skipVersionCheck = false;
  final positional = <String>[];

  for (final arg in arguments) {
    switch (arg) {
      case '--version':
        stdout.writeln('dcb_gen_tool $_version');
        return 0;
      case '--help':
      case '-h':
        stdout.write(_usage);
        return 0;
      case '--force':
        force = true;
      case '--quiet':
        quiet = true;
      case '--skip-version-check':
        skipVersionCheck = true;
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
        return await runGenerate(args,
            force: force, quiet: quiet, skipVersionCheck: skipVersionCheck);
      case 'init':
        return await cmdInit(args, force: force, quiet: quiet);
      case 'bootstrap':
        return await _cmdBootstrap(force: force, quiet: quiet);
      case 'doctor':
        return await _cmdDoctor(quiet: quiet);
      default:
        stderr.writeln('error: unknown command "$command"');
        stderr.writeln('Run "dcb_gen_tool --help" for usage.');
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

/// `dcb_gen_tool generate <config.yaml>`
Future<int> runGenerate(
  List<String> args, {
  required bool force,
  required bool quiet,
  required bool skipVersionCheck,
}) async {
  if (args.isEmpty) {
    stderr.writeln('error: missing <config.yaml> argument.');
    stderr.writeln('Usage: dcb_gen_tool generate <path/to/dart_cpp_bridge.yaml>');
    return 1;
  }

  final configPath = args.first;
  final configFile = File(configPath);
  if (!configFile.existsSync()) {
    stderr.writeln('error: config file not found: $configPath');
    return 1;
  }

  // --- Version consistency check ---
  if (!skipVersionCheck) {
    final versionError =
        _checkRuntimeVersion(p.dirname(p.absolute(configPath)));
    if (versionError != null) {
      stderr.writeln(versionError);
      return 1;
    }
  } else {
    stderr.writeln(
        '[dcb_gen_tool] WARNING: --skip-version-check is set; '
        'generated code may be incompatible with the runtime.');
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

  final exitCode = await process.exitCode;
  if (exitCode != 0) return exitCode;

  // Post-process generated sources.
  final configContent = File(absConfig).readAsStringSync();
  final yamlConfig = loadYaml(configContent) as YamlMap;
  await _postProcessDartOutput(absConfig, yamlConfig, log);
  await _postProcessCppOutput(absConfig, yamlConfig, log);
  return 0;
}

/// Runs `dart fix --apply` + `dart format` on the generated Dart output
/// directory so that emitted code is always analysis-clean and formatted.
Future<void> _postProcessDartOutput(
    String absConfig, YamlMap config, CliLogger log) async {
  final projectDir = p.dirname(absConfig);
  final dartOutputRel = config['dart_output'] as String?;
  if (dartOutputRel == null) {
    log.warn('dart_output not found in config; skipping dart fix/format.');
    return;
  }

  final dartOutput = p.normalize(p.join(projectDir, dartOutputRel));
  if (!Directory(dartOutput).existsSync()) {
    log.warn('Dart output directory not found: $dartOutput');
    return;
  }

  final dartExe = Platform.resolvedExecutable;

  log.info('Running dart fix ...');
  final fix = await Process.run(
    dartExe,
    ['fix', '--apply', dartOutput],
    workingDirectory: projectDir,
  );
  if (fix.exitCode != 0) {
    log.warn('dart fix exited with ${fix.exitCode}: ${fix.stderr}');
  }

  log.info('Running dart format ...');
  final fmt = await Process.run(
    dartExe,
    ['format', dartOutput],
    workingDirectory: projectDir,
  );
  if (fmt.exitCode != 0) {
    log.warn('dart format exited with ${fmt.exitCode}: ${fmt.stderr}');
  }
}

/// Runs clang-format on generated C++ sources when available.
///
/// Executable resolution order: each entry in the `clang_format` list in the
/// YAML config (tried top-to-bottom) → `clang-format` on PATH → skip.
Future<void> _postProcessCppOutput(
    String absConfig, YamlMap config, CliLogger log) async {
  final projectDir = p.dirname(absConfig);

  final cppOutputRel = config['cpp_wire_output'] as String?;
  if (cppOutputRel == null) return;
  final cppOutput = p.normalize(p.join(projectDir, cppOutputRel));
  if (!Directory(cppOutput).existsSync()) return;

  final clangFormat = _resolveClangFormat(config, projectDir, log);
  if (clangFormat == null) {
    log.info('clang-format not found; skipping C++ formatting.');
    return;
  }

  final sources = Directory(cppOutput)
      .listSync()
      .whereType<File>()
      .where((f) =>
          f.path.endsWith('.cpp') ||
          f.path.endsWith('.hpp') ||
          f.path.endsWith('.h'))
      .map((f) => f.path)
      .toList();
  if (sources.isEmpty) return;

  // Use the project's .clang-format when present (searched upward from the
  // output directory, mirroring clang-format's own lookup); otherwise LLVM.
  final style = _findDotClangFormat(cppOutput) != null ? 'file' : 'LLVM';

  log.info('Running clang-format (--style=$style) ...');
  final result = await Process.run(
    clangFormat,
    ['-i', '--style=$style', ...sources],
    workingDirectory: projectDir,
  );
  if (result.exitCode != 0) {
    log.warn('clang-format exited with ${result.exitCode}: ${result.stderr}');
  }
}

/// Resolves the clang-format executable.
///
/// Resolution order:
/// 1. Each entry in the `clang_format` list (top-to-bottom). Each entry can
///    be a direct executable path or a containing directory.
/// 2. `clang-format` on system PATH.
/// 3. Returns null (skip formatting).
String? _resolveClangFormat(
    YamlMap config, String projectDir, CliLogger log) {
  final candidates = _clangFormatCandidates(config);
  for (final entry in candidates) {
    var resolved = p.isAbsolute(entry)
        ? entry
        : p.normalize(p.join(projectDir, entry));
    // If it's a directory, look for the executable inside.
    if (Directory(resolved).existsSync()) {
      resolved = p.join(
          resolved, Platform.isWindows ? 'clang-format.exe' : 'clang-format');
    }
    if (File(resolved).existsSync()) return resolved;
    log.info('clang_format candidate not found: $resolved');
  }
  // Fall back to system PATH.
  return _which('clang-format');
}

/// Extracts the `clang_format` config value as a list of candidate paths.
List<String> _clangFormatCandidates(YamlMap config) {
  final raw = config['clang_format'];
  if (raw == null) return [];
  if (raw is YamlList) {
    return raw.map((e) => e.toString()).toList();
  }
  return [];
}

/// Searches upward from [dir] for a `.clang-format` file, mirroring
/// clang-format's own config lookup.
String? _findDotClangFormat(String dir) {
  var current = p.normalize(p.absolute(dir));
  while (true) {
    if (File(p.join(current, '.clang-format')).existsSync()) {
      return p.join(current, '.clang-format');
    }
    final parent = p.dirname(current);
    if (parent == current) return null;
    current = parent;
  }
}

/// `dcb_gen_tool bootstrap`
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

/// `dcb_gen_tool doctor`
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
    log.info('Cached toolchain: no (run "dcb_gen_tool bootstrap")');
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

/// Checks that the target project's resolved `dart_cpp_bridge` version matches
/// this tool's version. Returns an error message string on mismatch, or `null`
/// when the check passes.
String? _checkRuntimeVersion(String projectDir) {
  final lockPath = p.join(projectDir, 'pubspec.lock');
  final lockFile = File(lockPath);
  if (!lockFile.existsSync()) {
    return 'error: pubspec.lock not found in $projectDir\n'
        'Run "dart pub get" (or "flutter pub get") first.';
  }

  final content = lockFile.readAsStringSync();
  final lock = loadYaml(content) as YamlMap;
  final packages = lock['packages'] as YamlMap?;
  if (packages == null || !packages.containsKey('dart_cpp_bridge')) {
    return 'error: dart_cpp_bridge not found in pubspec.lock\n'
        'The target project does not depend on package:dart_cpp_bridge.';
  }

  final entry = packages['dart_cpp_bridge'] as YamlMap;
  final resolved = entry['version'] as String?;
  if (resolved == null) {
    return 'error: could not read dart_cpp_bridge version from pubspec.lock';
  }

  if (resolved != _version) {
    return 'error: dart_cpp_bridge version mismatch.\n'
        '  dcb_gen_tool expects: $_version\n'
        '  project resolved:     $resolved\n'
        '\n'
        'Run "dart pub upgrade dart_cpp_bridge" to align versions,\n'
        'or pass --skip-version-check to bypass (not recommended).';
  }

  return null;
}

/// Simple CLI logger that writes to stderr.
class CliLogger {
  final bool quiet;
  CliLogger({required this.quiet});

  void info(String msg) {
    if (!quiet) stderr.writeln('[dcb_gen_tool] $msg');
  }

  void warn(String msg) {
    stderr.writeln('[dcb_gen_tool] WARNING: $msg');
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
