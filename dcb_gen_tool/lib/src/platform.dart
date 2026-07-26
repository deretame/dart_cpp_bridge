import 'dart:ffi';
import 'dart:io';

import 'package:path/path.dart' as p;

/// Detected host platform information used to select the correct
/// python-build-standalone and libclang-ng wheel from versions.lock.
class HostPlatform {
  /// One of: `windows`, `linux`, `macos`.
  final String os;

  /// One of: `x86_64`, `aarch64`.
  final String arch;

  /// Lock-file key, e.g. `windows-x86_64`, `linux-aarch64`.
  late final String key = '$os-$arch';

  HostPlatform._(this.os, this.arch);

  /// Detect the current host platform.
  factory HostPlatform.detect() {
    final String os;
    if (Platform.isWindows) {
      os = 'windows';
    } else if (Platform.isMacOS) {
      os = 'macos';
    } else if (Platform.isLinux) {
      os = 'linux';
    } else {
      throw UnsupportedError(
          'Unsupported host OS: ${Platform.operatingSystem}');
    }

    final String arch;
    final abi = Abi.current();
    if (abi == Abi.windowsX64 || abi == Abi.linuxX64 || abi == Abi.macosX64) {
      arch = 'x86_64';
    } else if (abi == Abi.windowsArm64 ||
        abi == Abi.linuxArm64 ||
        abi == Abi.macosArm64) {
      arch = 'aarch64';
    } else {
      throw UnsupportedError('Unsupported host architecture: $abi');
    }

    return HostPlatform._(os, arch);
  }

  bool get isWindows => os == 'windows';
  bool get isMacOS => os == 'macos';
  bool get isLinux => os == 'linux';

  /// The python executable name inside the extracted toolchain.
  String get pythonExeName => isWindows ? 'python.exe' : 'python3';

  /// Resolve the toolchain cache root directory.
  ///
  /// Priority:
  ///   1. `DCB_CODEGEN_CACHE` environment variable
  ///   2. Platform default:
  ///      - Windows: `%LOCALAPPDATA%\dart_cpp_bridge\toolchain`
  ///      - macOS:   `~/Library/Caches/dart_cpp_bridge/toolchain`
  ///      - Linux:   `${XDG_CACHE_HOME:-~/.cache}/dart_cpp_bridge/toolchain`
  String resolveCacheRoot() {
    final env = Platform.environment;

    final override = env['DCB_CODEGEN_CACHE'];
    if (override != null && override.isNotEmpty) {
      return p.normalize(override);
    }

    if (isWindows) {
      final localAppData = env['LOCALAPPDATA'];
      if (localAppData == null || localAppData.isEmpty) {
        throw StateError(
            'LOCALAPPDATA is not set; cannot resolve toolchain cache path. '
            'Set DCB_CODEGEN_CACHE to override.');
      }
      return p.join(localAppData, 'dart_cpp_bridge', 'toolchain');
    }

    final home = env['HOME'] ?? env['USERPROFILE'];
    if (home == null || home.isEmpty) {
      throw StateError(
          'Cannot determine HOME directory for toolchain cache. '
          'Set DCB_CODEGEN_CACHE to override.');
    }

    if (isMacOS) {
      return p.join(home, 'Library', 'Caches', 'dart_cpp_bridge', 'toolchain');
    }

    // Linux
    final xdg = env['XDG_CACHE_HOME'];
    final cacheBase =
        (xdg != null && xdg.isNotEmpty) ? xdg : p.join(home, '.cache');
    return p.join(cacheBase, 'dart_cpp_bridge', 'toolchain');
  }
}
