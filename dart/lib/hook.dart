/// Build-hook support for **dart_cpp_bridge** Native Assets.
///
/// Provides a platform-configuration sealed hierarchy ([DcbPlatformConfig]) and
/// a reusable CMake-driven builder ([DcbCMakeBuilder]) that downstream packages
/// use from their `hook/build.dart` to compile a native library and emit it as
/// a bundled [CodeAsset], consumable at runtime via
/// `@Native(assetId: 'package:<package>/<assetName>')`.
///
/// Only Windows is implemented in this phase; the remaining platform configs
/// are placeholders and [DcbCMakeBuilder.run] throws [UnsupportedError] for
/// them.
///
/// This library is intentionally separate from `package:dart_cpp_bridge`
/// (the runtime FFI front-end): it depends on `package:hooks` /
/// `package:code_assets`, which are only needed inside a build hook.
library;

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';

/// Platform-specific configuration for [DcbCMakeBuilder].
///
/// A downstream `hook/build.dart` selects the appropriate subclass from
/// `input.config.code.targetOS` and passes it to [DcbCMakeBuilder]:
///
/// ```dart
/// final config = switch (input.config.code.targetOS) {
///   OS.windows => const WindowsConfig(),
///   final os => throw UnsupportedError('unsupported target OS: $os'),
/// };
/// await DcbCMakeBuilder(config: config, assetName: 'my_package.dart')
///     .run(input: input, output: output);
/// ```
sealed class DcbPlatformConfig {
  const DcbPlatformConfig();

  /// The CMake executable to invoke (resolved via `PATH` by default).
  String get cmake;
}

/// Windows configuration for [DcbCMakeBuilder].
final class WindowsConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// Optional CMake generator (`-G`). When `null`, CMake picks its default,
  /// which on Windows is typically a multi-config Visual Studio generator.
  final String? generator;

  const WindowsConfig({this.cmake = 'cmake', this.generator});
}

/// Linux configuration (placeholder; not yet supported by [DcbCMakeBuilder]).
final class LinuxConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  const LinuxConfig({this.cmake = 'cmake'});
}

/// Android configuration (placeholder; not yet supported by [DcbCMakeBuilder]).
final class AndroidConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// Optional path to the Android NDK (forwarded as `CMAKE_ANDROID_NDK`).
  final String? ndkPath;

  /// Optional minimum Android API level (forwarded as `ANDROID_PLATFORM`).
  final int? androidPlatform;

  const AndroidConfig({this.cmake = 'cmake', this.ndkPath, this.androidPlatform});
}

/// macOS configuration (placeholder; not yet supported by [DcbCMakeBuilder]).
final class MacosConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// Whether to build a universal (arm64 + x86_64) binary.
  final bool universal;

  const MacosConfig({this.cmake = 'cmake', this.universal = false});
}

/// iOS configuration (placeholder; not yet supported by [DcbCMakeBuilder]).
final class IosConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  const IosConfig({this.cmake = 'cmake'});
}

/// Thrown when a CMake invocation or artifact lookup fails in [DcbCMakeBuilder].
final class DcbCMakeException implements Exception {
  final String message;

  const DcbCMakeException(this.message);

  @override
  String toString() => 'DcbCMakeException: $message';
}

/// A reusable build-hook builder that drives CMake to compile a native library
/// and emits it as a bundled [CodeAsset].
///
/// The builder performs three CMake steps inside
/// `input.outputDirectory/dcb_build/`:
///
/// 1. `cmake -S <sourceDir> -B <buildDir> [generator] [extraDefines]`
/// 2. `cmake --build <buildDir> --config Release`
/// 3. locate the produced dynamic library and emit it as a [CodeAsset].
final class DcbCMakeBuilder {
  /// The platform configuration selecting CMake behavior.
  final DcbPlatformConfig config;

  /// The code-asset name within the package, e.g. `'hook_demo.dart'`.
  ///
  /// This becomes the runtime identifier
  /// `@Native(assetId: 'package:<package>/<assetName>')`.
  final String assetName;

  /// The CMake source directory (`cmake -S`), relative to the package root.
  /// Defaults to the package root.
  final String? sourceDir;

  /// The expected library base name (CMake output name). Defaults to the
  /// package name.
  final String? libName;

  /// Extra definitions passed verbatim to the CMake configure step, e.g.
  /// `['-DDCB_BUILD_SHARED=ON']`.
  final List<String> extraDefines;

  const DcbCMakeBuilder({
    required this.config,
    required this.assetName,
    this.sourceDir,
    this.libName,
    this.extraDefines = const [],
  });

  /// Runs the CMake build and emits the produced library as a bundled
  /// [CodeAsset].
  ///
  /// No-op when [HookConfigCodeConfig.buildCodeAssets] is false.
  Future<void> run({
    required BuildInput input,
    required BuildOutputBuilder output,
  }) async {
    if (!input.config.buildCodeAssets) {
      return;
    }

    final targetOS = input.config.code.targetOS;
    final packageName = input.packageName;
    final libBaseName = libName ?? packageName;

    final sourceRoot = sourceDir == null
        ? input.packageRoot
        : input.packageRoot.resolve('$sourceDir/');
    final buildDir = input.outputDirectory.resolve('dcb_build/');
    Directory.fromUri(buildDir).createSync(recursive: true);

    // Resolve the CMake executable and platform-specific configure arguments.
    final String cmake;
    final configureArgs = <String>[];
    switch (config) {
      case WindowsConfig(:final generator):
        cmake = _resolveWindowsCmake(config.cmake);
        // Default generator: CMake auto-selects (typically Visual Studio).
        if (generator case final g?) {
          configureArgs.addAll(['-G', g]);
        }
      case LinuxConfig():
        throw UnsupportedError('DcbCMakeBuilder: Linux is not supported yet.');
      case AndroidConfig():
        throw UnsupportedError(
          'DcbCMakeBuilder: Android is not supported yet.',
        );
      case MacosConfig():
        throw UnsupportedError('DcbCMakeBuilder: macOS is not supported yet.');
      case IosConfig():
        throw UnsupportedError('DcbCMakeBuilder: iOS is not supported yet.');
    }

    // 1. Configure.
    await _runProcess(cmake, [
      '-S',
      sourceRoot.toFilePath(),
      '-B',
      buildDir.toFilePath(),
      ...configureArgs,
      ...extraDefines,
    ]);

    // 2. Build. Multi-config generators honor `--config`; single-config
    //    generators ignore it.
    await _runProcess(cmake, [
      '--build',
      buildDir.toFilePath(),
      '--config',
      'Release',
    ]);

    // 3. Locate the produced dynamic library.
    final libFileName = targetOS.dylibFileName(libBaseName);
    final libFile = _locateArtifact(buildDir, libFileName);

    // 4. Declare cache dependencies (CMakeLists + native sources).
    _declareDependencies(sourceRoot, output);

    // 5. Emit the bundled code asset.
    output.assets.code.add(
      CodeAsset(
        package: packageName,
        name: assetName,
        linkMode: DynamicLoadingBundled(),
        file: libFile.uri,
      ),
    );
  }

  /// Runs [executable] with [arguments], throwing [DcbCMakeException] on a
  /// non-zero exit code.
  Future<void> _runProcess(String executable, List<String> arguments) async {
    _log('$executable ${arguments.join(' ')}');
    final ProcessResult result;
    try {
      result = await Process.run(executable, arguments);
    } on ProcessException catch (e) {
      throw DcbCMakeException(
        'Failed to invoke "$executable" (is CMake installed and on PATH?): $e',
      );
    }
    if (result.exitCode != 0) {
      throw DcbCMakeException(
        '"$executable ${arguments.join(' ')}" exited with code '
        '${result.exitCode}.\n'
        'stdout:\n${result.stdout}\n'
        'stderr:\n${result.stderr}',
      );
    }
  }

  /// Locates the built [fileName] under [buildDir], searching the common
  /// multi-config (`Release/`, `Debug/`) and single-config (root) layouts.
  File _locateArtifact(Uri buildDir, String fileName) {
    final candidates = <Uri>[
      buildDir.resolve('Release/').resolve(fileName),
      buildDir.resolve('Debug/').resolve(fileName),
      buildDir.resolve(fileName),
    ];
    for (final candidate in candidates) {
      final file = File.fromUri(candidate);
      if (file.existsSync()) {
        return file;
      }
    }
    throw DcbCMakeException(
      'Could not locate built library "$fileName" under '
      '"${buildDir.toFilePath()}". Searched:\n'
      '${candidates.map((c) => '  - ${c.toFilePath()}').join('\n')}',
    );
  }

  /// Declares the native sources and CMake scripts under [sourceRoot] as hook
  /// dependencies so the build cache invalidates when they change.
  void _declareDependencies(Uri sourceRoot, BuildOutputBuilder output) {
    final rootDir = Directory.fromUri(sourceRoot);
    if (!rootDir.existsSync()) {
      return;
    }
    const sourceExtensions = {
      '.c', '.cc', '.cpp', '.cxx', '.c++',
      '.h', '.hh', '.hpp', '.hxx', '.h++',
      '.m', '.mm', '.cmake',
    };
    for (final entity in rootDir.listSync(recursive: true, followLinks: false)) {
      if (entity is! File) {
        continue;
      }
      // Skip nested build/output directories.
      if (entity.path.contains('dcb_build')) {
        continue;
      }
      final name = entity.uri.pathSegments.last;
      final dot = name.lastIndexOf('.');
      final ext = dot < 0 ? '' : name.substring(dot).toLowerCase();
      if (name == 'CMakeLists.txt' || sourceExtensions.contains(ext)) {
        output.dependencies.add(entity.uri);
      }
    }
  }

  /// Resolves the CMake executable on Windows.
  ///
  /// When [configured] is the default bare `cmake`, first trusts `PATH`, then
  /// falls back to discovering a CMake bundled with Visual Studio (via
  /// `vswhere.exe`) or a standalone install. An explicit path/name is used
  /// verbatim.
  String _resolveWindowsCmake(String configured) {
    if (configured != 'cmake') {
      return configured;
    }
    if (_isOnPath('cmake.exe')) {
      return configured;
    }
    for (final candidate in _windowsCmakeCandidates()) {
      if (File(candidate).existsSync()) {
        _log('resolved CMake: $candidate');
        return candidate;
      }
    }
    // Fall back to the bare name and rely on PATH at invocation time.
    return configured;
  }

  /// Whether [exeName] resolves against a directory listed in `PATH`.
  bool _isOnPath(String exeName) {
    final pathVar = Platform.environment['PATH'] ?? '';
    for (final dir in pathVar.split(';')) {
      if (dir.isEmpty) {
        continue;
      }
      if (File('$dir\\$exeName').existsSync()) {
        return true;
      }
    }
    return false;
  }

  /// Candidate absolute paths to a Windows CMake executable, most-reliable
  /// first: standalone installs, then Visual Studio (via `vswhere.exe` and a
  /// well-known layout probe).
  Iterable<String> _windowsCmakeCandidates() sync* {
    const cmakeRelative =
        r'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe';
    yield r'C:\Program Files\CMake\bin\cmake.exe';
    yield r'C:\Program Files (x86)\CMake\bin\cmake.exe';

    // vswhere.exe is the official VS locator (PROGRAMDATA is passed through to
    // hooks specifically for it).
    const vswhere =
        r'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe';
    if (File(vswhere).existsSync()) {
      try {
        final result = Process.runSync(vswhere, [
          '-latest',
          '-products',
          '*',
          '-property',
          'installationPath',
        ]);
        if (result.exitCode == 0) {
          for (final line in (result.stdout as String).split(
            RegExp(r'[\r\n]+'),
          )) {
            final installPath = line.trim();
            if (installPath.isNotEmpty) {
              yield '$installPath\\$cmakeRelative';
            }
          }
        }
      } on ProcessException {
        // Ignore and fall through to the layout probe.
      }
    }

    // Well-known VS 2017+ layout: <root>\<version>\<edition>\...
    const roots = [
      r'C:\Program Files\Microsoft Visual Studio',
      r'C:\Program Files (x86)\Microsoft Visual Studio',
    ];
    for (final root in roots) {
      final rootDir = Directory(root);
      if (!rootDir.existsSync()) {
        continue;
      }
      for (final versionDir in rootDir.listSync().whereType<Directory>()) {
        for (final editionDir in versionDir.listSync().whereType<Directory>()) {
          yield '${editionDir.path}\\$cmakeRelative';
        }
      }
    }
  }

  void _log(String message) {
    // Hook stdout is captured to stdout.txt and surfaced on failure.
    stdout.writeln('[dcb] $message');
  }
}
