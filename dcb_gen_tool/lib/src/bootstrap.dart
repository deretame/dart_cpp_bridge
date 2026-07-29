import 'dart:convert';
import 'dart:io';

import 'package:archive/archive.dart';
import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;
import 'package:path/path.dart' as p;

import 'lock_file.dart';
import 'platform.dart';

/// Result of a successful bootstrap.
class BootstrapResult {
  /// Absolute path to the python executable inside the cached toolchain.
  final String pythonExe;

  /// The environment key (fingerprint) identifying this toolchain snapshot.
  final String envKey;

  const BootstrapResult({required this.pythonExe, required this.envKey});
}

/// Options controlling bootstrap behaviour.
class BootstrapOptions {
  /// Force re-extraction even if READY.json matches.
  final bool force;

  /// Suppress non-error output.
  final bool quiet;

  const BootstrapOptions({this.force = false, this.quiet = false});
}

/// Ensure the locked Python toolchain is available locally.
///
/// Downloads python-build-standalone + libclang-ng + ruamel.yaml wheels
/// (verifying SHA-256), extracts them into a content-addressed environment
/// directory, runs smoke tests, and returns the path to the python
/// executable.
Future<BootstrapResult> bootstrap(
  HostPlatform platform,
  LockFile lock, {
  required String cacheRoot,
  required String packageRoot,
  BootstrapOptions options = const BootstrapOptions(),
}) async {
  final log = _Logger(quiet: options.quiet);

  final pyEntry = lock.pythonFor(platform.key);
  final lcEntry = lock.libclangFor(platform.key);
  final ryEntry = lock.ruamel;

  // --- 1. Compute environment fingerprint -------------------------------
  final fingerprintInput =
      '${platform.key}|${pyEntry.sha256}|${lcEntry.sha256}|${ryEntry.sha256}';
  final envKey =
      sha256.convert(utf8.encode(fingerprintInput)).toString().substring(0, 16);

  final envDir = Directory(p.join(cacheRoot, 'envs', envKey));
  final readyFile = File(p.join(envDir.path, 'READY.json'));
  final lastEnvFile = File(p.join(cacheRoot, 'LAST_ENV.json'));

  // --- 2. Check existing environment ------------------------------------
  if (!options.force && readyFile.existsSync()) {
    try {
      final ready =
          jsonDecode(readyFile.readAsStringSync()) as Map<String, dynamic>;
      if (ready['env_key'] == envKey &&
          ready['python_sha256'] == pyEntry.sha256 &&
          ready['libclang_sha256'] == lcEntry.sha256 &&
          ready['ruamel_sha256'] == ryEntry.sha256) {
        final pythonExe = _findPythonExe(envDir.path, platform);
        if (File(pythonExe).existsSync()) {
          log.info('Reusing existing toolchain env: $envKey');
          _writeLastEnv(lastEnvFile, envKey, envDir.path);
          return BootstrapResult(pythonExe: pythonExe, envKey: envKey);
        }
      }
    } catch (_) {
      // Corrupted stamp — fall through to rebuild.
    }
  }

  // --- 3. Download artifacts --------------------------------------------
  final downloadsDir = Directory(p.join(cacheRoot, 'downloads'))
    ..createSync(recursive: true);

  log.info('Bootstrapping codegen toolchain (env: $envKey) ...');

  final pyFile = await _download(
      pyEntry, downloadsDir.path, 'python-build-standalone', log);
  final lcFile =
      await _download(lcEntry, downloadsDir.path, 'libclang-ng wheel', log);
  final ryFile = await _download(
      ryEntry, downloadsDir.path, 'ruamel.yaml wheel', log);

  // --- 4. Extract -------------------------------------------------------
  // Clean previous partial env.
  if (envDir.existsSync()) {
    envDir.deleteSync(recursive: true);
  }
  envDir.createSync(recursive: true);

  // 4a. Python tar.gz
  log.info('Extracting python-build-standalone ...');
  final pythonDir = p.join(envDir.path, 'python');
  _extractTarGz(pyFile, pythonDir);

  // 4b. libclang-ng wheel (zip) → site-packages
  log.info('Installing libclang-ng ${lock.libclangVersion} ...');
  final sitePackages = _findSitePackages(pythonDir, platform);
  _extractWheel(lcFile, sitePackages);

  // 4c. ruamel.yaml wheel (zip) → site-packages
  log.info('Installing ruamel.yaml ${lock.ruamelVersion} ...');
  _extractWheel(ryFile, sitePackages);

  // 4d. Ensure executable permissions on Unix.
  if (!platform.isWindows) {
    _ensureExecutable(pythonDir, platform);
  }

  // --- 5. Locate python exe & prepare runtime ----------------------------
  final pythonExe = _findPythonExe(envDir.path, platform);
  if (!File(pythonExe).existsSync()) {
    throw StateError(
        'python executable not found after extraction: $pythonExe');
  }

  // 5a. Windows: ensure msvcp140.dll (C++ stdlib runtime) is available.
  //     python-build-standalone ships vcruntime140.dll but NOT msvcp140.dll.
  //     libclang.dll (C++) requires it.
  if (platform.isWindows) {
    await _ensureMsvcp(pythonExe, cacheRoot, log);
  }

  // --- 6. Smoke test ----------------------------------------------------
  log.info('Smoke-testing libclang ...');
  _smokeTest(pythonExe,
      'from clang.cindex import Index; idx = Index.create(); assert idx is not None; print("libclang OK")');

  log.info('Smoke-testing ruamel.yaml ...');
  _smokeTest(pythonExe,
      'from ruamel.yaml import YAML; y = YAML(); assert y is not None; print("ruamel.yaml OK")');

  // --- 6. Write stamps --------------------------------------------------
  final now = DateTime.now().toUtc().toIso8601String();
  readyFile.writeAsStringSync(const JsonEncoder.withIndent('  ').convert({
    'env_key': envKey,
    'platform': platform.key,
    'python_version': lock.pythonVersion,
    'python_sha256': pyEntry.sha256,
    'libclang_version': lock.libclangVersion,
    'libclang_sha256': lcEntry.sha256,
    'ruamel_version': lock.ruamelVersion,
    'ruamel_sha256': ryEntry.sha256,
    'created_at': now,
  }));
  _writeLastEnv(lastEnvFile, envKey, envDir.path);

  log.info('Toolchain ready: $pythonExe');
  return BootstrapResult(pythonExe: pythonExe, envKey: envKey);
}

// ===========================================================================
// Internal helpers
// ===========================================================================

class _Logger {
  final bool quiet;
  _Logger({required this.quiet});
  void info(String msg) {
    if (!quiet) stderr.writeln('[codegen] $msg');
  }
}

/// Collects the final [Digest] emitted by a chunked hash conversion.
class _DigestCollector implements Sink<Digest> {
  Digest? _digest;
  Digest get digest => _digest ?? (throw StateError('Hash not finalized'));

  @override
  void add(Digest data) => _digest = data;

  @override
  void close() {}
}

/// Download [entry] into [destDir] using content-addressed file name.
/// Skips download if the file already exists with matching SHA-256.
Future<File> _download(
    ArtifactEntry entry, String destDir, String label, _Logger log) async {
  final fileName = entry.contentFileName;
  final dest = File(p.join(destDir, fileName));

  if (dest.existsSync() && _verifySha256(dest, entry.sha256)) {
    log.info('$label: cache hit ($fileName)');
    return dest;
  }

  log.info('$label: downloading $fileName ...');
  final client = http.Client();
  try {
    final request = http.Request('GET', Uri.parse(entry.url));
    final response = await client.send(request);
    if (response.statusCode != 200) {
      throw HttpException(
          'HTTP ${response.statusCode} downloading $label from ${entry.url}');
    }

    // Stream to file while computing SHA-256 incrementally.
    final hashCollector = _DigestCollector();
    final hashInput = sha256.startChunkedConversion(hashCollector);
    final fileSink = dest.openWrite();
    try {
      await for (final chunk in response.stream) {
        fileSink.add(chunk);
        hashInput.add(chunk);
      }
    } finally {
      hashInput.close();
      await fileSink.flush();
      await fileSink.close();
    }

    // Verify hash.
    final digest = hashCollector.digest;
    if (digest.toString().toLowerCase() != entry.sha256.toLowerCase()) {
      dest.deleteSync();
      throw StateError(
          'SHA-256 mismatch after downloading $label ($fileName). '
          'Expected: ${entry.sha256}, got: $digest. '
          'File deleted; re-run to retry.');
    }

    log.info('$label: verified (${entry.sha256.substring(0, 12)}...)');
    return dest;
  } finally {
    client.close();
  }
}

/// Verify a file's SHA-256 against [expected] (hex string).
bool _verifySha256(File file, String expected) {
  final digest = sha256.convert(file.readAsBytesSync());
  return digest.toString().toLowerCase() == expected.toLowerCase();
}

/// Extract a .tar.gz archive into [destDir].
///
/// If all entries share a common top-level directory prefix (e.g. `python/`),
/// that prefix is stripped so the contents land directly in [destDir].
void _extractTarGz(File tarGzFile, String destDir) {
  final bytes = tarGzFile.readAsBytesSync();
  final decompressed = GZipDecoder().decodeBytes(bytes);
  final archive = TarDecoder().decodeBytes(decompressed);

  // Detect common top-level prefix (e.g. "python/").
  final prefix = _commonTopLevelDir(archive);

  for (final entry in archive) {
    var name = entry.name;
    // Strip the common prefix.
    if (prefix.isNotEmpty && name.startsWith(prefix)) {
      name = name.substring(prefix.length);
    }
    if (name.isEmpty || name == '.' || name == './') continue;

    final outPath = p.join(destDir, name);
    // Guard against path traversal.
    if (!p.isWithin(destDir, outPath) && p.normalize(outPath) != destDir) {
      continue;
    }
    if (entry.isSymbolicLink) {
      // Handle symlinks (e.g. python3 -> python3.13).
      // Must check before isFile because symlinks also report isFile=true.
      final link = Link(outPath);
      link.parent.createSync(recursive: true);
      final target = entry.symbolicLink;
      if (target != null && target.isNotEmpty) {
        try {
          link.createSync(target);
        } catch (_) {
          // If symlink creation fails, copy the target file content.
          final targetPath = p.join(p.dirname(outPath), target);
          if (File(targetPath).existsSync()) {
            File(outPath).writeAsBytesSync(File(targetPath).readAsBytesSync());
          }
        }
      }
    } else if (entry.isFile) {
      final outFile = File(outPath);
      outFile.parent.createSync(recursive: true);
      outFile.writeAsBytesSync(entry.content as List<int>);
    } else {
      Directory(outPath).createSync(recursive: true);
    }
  }
}

/// Detect a common top-level directory prefix in archive entries.
/// Returns e.g. `"python/"` if all entries start with that, or `""` if none.
String _commonTopLevelDir(Archive archive) {
  String? prefix;
  for (final entry in archive) {
    final name = entry.name.replaceAll('\\', '/');
    final slashIdx = name.indexOf('/');
    if (slashIdx <= 0) return ''; // No common prefix.
    final top = name.substring(0, slashIdx + 1);
    if (prefix == null) {
      prefix = top;
    } else if (prefix != top) {
      return ''; // Different top-level dirs.
    }
  }
  return prefix ?? '';
}

/// Extract a wheel (.whl = zip) into [destDir] (site-packages).
void _extractWheel(File wheelFile, String destDir) {
  final bytes = wheelFile.readAsBytesSync();
  final archive = ZipDecoder().decodeBytes(bytes);

  for (final entry in archive) {
    final outPath = p.join(destDir, entry.name);
    if (!p.isWithin(destDir, outPath) && p.normalize(outPath) != destDir) {
      continue;
    }
    if (entry.isFile) {
      // Skip .dist-info metadata to keep site-packages lean.
      if (entry.name.contains('.dist-info/')) continue;
      final outFile = File(outPath);
      outFile.parent.createSync(recursive: true);
      outFile.writeAsBytesSync(entry.content as List<int>);
    } else {
      Directory(outPath).createSync(recursive: true);
    }
  }
}

/// Find the site-packages directory inside the extracted python tree.
String _findSitePackages(String pythonDir, HostPlatform platform) {
  // python-build-standalone layout (install_only_stripped, prefix stripped):
  //   Windows: python/Lib/site-packages
  //   Unix:    python/lib/python3.X/site-packages
  if (platform.isWindows) {
    final primary = p.join(pythonDir, 'Lib', 'site-packages');
    if (Directory(primary).existsSync()) return primary;
    // Fallback: nested python/python/Lib/site-packages
    final nested = p.join(pythonDir, 'python', 'Lib', 'site-packages');
    if (Directory(nested).existsSync()) return nested;
  } else {
    // Unix: python/lib/python3.X/site-packages
    final libDir = Directory(p.join(pythonDir, 'lib'));
    if (libDir.existsSync()) {
      for (final child in libDir.listSync()) {
        if (child is Directory &&
            p.basename(child.path).startsWith('python3')) {
          final sp = p.join(child.path, 'site-packages');
          if (Directory(sp).existsSync()) return sp;
        }
      }
    }
  }
  // Create it if not found.
  final created = platform.isWindows
      ? p.join(pythonDir, 'Lib', 'site-packages')
      : p.join(pythonDir, 'lib', 'python3', 'site-packages');
  Directory(created).createSync(recursive: true);
  return created;
}

/// Locate the python executable inside the env directory.
String _findPythonExe(String envDir, HostPlatform platform) {
  final pythonRoot = p.join(envDir, 'python');
  if (platform.isWindows) {
    // After prefix-stripping: python/python.exe
    final candidates = [
      p.join(pythonRoot, 'python.exe'),
      p.join(pythonRoot, 'python', 'python.exe'),
      p.join(pythonRoot, 'install', 'python.exe'),
    ];
    for (final c in candidates) {
      if (File(c).existsSync()) return c;
    }
  } else {
    // Unix: python/bin/python3
    final candidates = [
      p.join(pythonRoot, 'bin', 'python3'),
      p.join(pythonRoot, 'python', 'bin', 'python3'),
      p.join(pythonRoot, 'install', 'bin', 'python3'),
    ];
    for (final c in candidates) {
      if (File(c).existsSync()) return c;
    }
  }
  // Fallback: return expected path (caller will check existence).
  return platform.isWindows
      ? p.join(pythonRoot, 'python.exe')
      : p.join(pythonRoot, 'bin', 'python3');
}

/// Ensure python3 and shared libs have executable permission on Unix.
void _ensureExecutable(String pythonDir, HostPlatform platform) {
  final candidates = [
    p.join(pythonDir, 'bin'),
    p.join(pythonDir, 'python', 'bin'),
  ];
  for (final binPath in candidates) {
    final binDir = Directory(binPath);
    if (!binDir.existsSync()) continue;
    for (final entity in binDir.listSync(recursive: true)) {
      if (entity is File) {
        try {
          Process.runSync('chmod', ['+x', entity.path]);
        } catch (_) {
          // Best-effort.
        }
      }
    }
  }
}

/// Run a python one-liner and throw on failure.
void _smokeTest(String pythonExe, String code) {
  final result = Process.runSync(pythonExe, ['-c', code]);
  if (result.exitCode != 0) {
    final stderr = result.stderr.toString();
    var hint = '';
    if (stderr.contains('1114') ||
        stderr.contains('DLL initialization routine failed')) {
      hint = '\n\nHint: This usually means the Visual C++ Redistributable is '
          'missing.\nInstall it from: https://aka.ms/vs/17/release/vc_redist.x64.exe';
    }
    throw StateError(
        'Smoke test failed (exit ${result.exitCode}):\n'
        'stdout: ${result.stdout}\n'
        'stderr: $stderr$hint');
  }
}

/// Write LAST_ENV.json for debugging.
void _writeLastEnv(File lastEnvFile, String envKey, String envDir) {
  lastEnvFile.parent.createSync(recursive: true);
  lastEnvFile.writeAsStringSync(
      const JsonEncoder.withIndent('  ').convert({
    'env_key': envKey,
    'env_dir': envDir,
    'updated_at': DateTime.now().toUtc().toIso8601String(),
  }));
}

// ---------------------------------------------------------------------------
// Windows msvcp140.dll provisioning
// ---------------------------------------------------------------------------

const _msvcpUrl =
    'https://raw.githubusercontent.com/deretame/dart_cpp_bridge/main/'
    'dcb_gen_tool/runtime/windows-x64/msvcp140.dll';
const _msvcpSha256 =
    '7c26614e1d733892c2deac7e245ce115504b1d80592dd0a01b08e3e5a55f89ca';

/// Ensure msvcp140.dll is present next to python.exe.
///
/// Downloads from GitHub (SHA-256 verified) and caches locally.
Future<void> _ensureMsvcp(
    String pythonExe, String cacheRoot, _Logger log) async {
  final exeDir = p.dirname(pythonExe);
  final target = File(p.join(exeDir, 'msvcp140.dll'));
  if (target.existsSync()) return;

  // Check local cache first.
  final cached = File(p.join(cacheRoot, 'downloads', 'msvcp140.dll'));
  if (cached.existsSync() && _verifySha256(cached, _msvcpSha256)) {
    cached.copySync(target.path);
    log.info('msvcp140.dll: restored from cache');
    return;
  }

  // Download from GitHub.
  log.info('msvcp140.dll: downloading ...');
  final client = http.Client();
  try {
    final response = await client.get(Uri.parse(_msvcpUrl));
    if (response.statusCode != 200) {
      log.info('WARNING: HTTP ${response.statusCode} downloading msvcp140.dll');
      return;
    }
    final bytes = response.bodyBytes;
    final digest = sha256.convert(bytes).toString().toLowerCase();
    if (digest != _msvcpSha256) {
      log.info('WARNING: msvcp140.dll SHA-256 mismatch: $digest');
      return;
    }
    // Cache and place.
    cached.parent.createSync(recursive: true);
    cached.writeAsBytesSync(bytes);
    target.writeAsBytesSync(bytes);
    log.info('msvcp140.dll: downloaded and verified');
  } catch (e) {
    log.info('WARNING: Failed to download msvcp140.dll: $e');
  } finally {
    client.close();
  }
}
