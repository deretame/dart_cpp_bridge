/// Build-hook support for **dart_cpp_bridge** Native Assets.
///
/// Provides a platform-configuration sealed hierarchy ([DcbPlatformConfig]) and
/// a reusable CMake-driven builder ([DcbCMakeBuilder]) that downstream packages
/// use from their `hook/build.dart` to compile a native library and emit it as
/// a bundled [CodeAsset], consumable at runtime via
/// `@Native(assetId: 'package:<package>/<assetName>')`.
///
/// Only Windows is fully implemented in this phase; the remaining platform
/// configs are placeholders and [DcbCMakeBuilder.run] throws [UnsupportedError]
/// for them.
///
/// This library is intentionally separate from `package:dart_cpp_bridge`
/// (the runtime FFI front-end): it depends on `package:hooks` /
/// `package:code_assets`, which are only needed inside a build hook.
library;

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';

// ---------------------------------------------------------------------------
// Generator selection
// ---------------------------------------------------------------------------

/// CMake build-system generator for Windows.
enum CmakeGenerator {
  /// Visual Studio multi-config generator (MSBuild).
  ///
  /// Produces `Release/` and `Debug/` subdirectories. This is the CMake
  /// default on Windows and requires no extra environment setup.
  msbuild,

  /// Ninja single-config generator.
  ///
  /// Faster incremental builds. Requires the MSVC environment to be
  /// initialized (the builder invokes `vcvarsall.bat` automatically).
  ninja,
}

// ---------------------------------------------------------------------------
// Platform configuration hierarchy
// ---------------------------------------------------------------------------

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

  /// Whether to link the MSVC C runtime dynamically (`/MD`).
  ///
  /// Defaults to `true`. When `true`, the produced DLL depends on external
  /// `MSVCP140.dll` / `VCRUNTIME140.dll`. Set to `false` for a fully
  /// self-contained DLL (`/MT`) with no external CRT dependency.
  final bool dynamicCrt;

  /// Whether to bundle the correct CRT DLLs next to the output library.
  ///
  /// Defaults to `true`. Only effective when [dynamicCrt] is `true`. The
  /// builder copies `MSVCP140.dll`, `VCRUNTIME140.dll`, and
  /// `VCRUNTIME140_1.dll` from the VS Redist directory into the same folder
  /// as the built DLL, ensuring the correct version is loaded at runtime
  /// regardless of what the end-user's system has installed.
  final bool bundleCrt;

  /// Path to the Visual Studio or Build Tools installation root.
  ///
  /// Example: `r'C:\Program Files\Microsoft Visual Studio\18\Community'`
  ///
  /// When `null` (default), the builder auto-detects via `vswhere.exe`,
  /// then falls back to well-known installation paths.
  final String? vsInstallPath;

  /// Target architecture passed to CMake (`-A`).
  ///
  /// Supported values: `'x64'`, `'arm64'`. Defaults to `'x64'`.
  final String architecture;

  /// CMake generator selection.
  ///
  /// When `null` (default), CMake auto-selects (typically the Visual Studio
  /// multi-config generator on Windows).
  final CmakeGenerator? generator;

  /// Explicit path to the generator executable.
  ///
  /// For [CmakeGenerator.ninja]: path to `ninja.exe` when it is not on PATH.
  /// For [CmakeGenerator.msbuild]: path to `MSBuild.exe` (rarely needed).
  /// When `null`, resolved automatically.
  final String? generatorPath;

  /// Extra definitions passed verbatim to the CMake configure step, e.g.
  /// `['-DCMAKE_PREFIX_PATH=C:/vcpkg']`.
  final List<String> extraDefines;

  const WindowsConfig({
    this.cmake = 'cmake',
    this.dynamicCrt = true,
    this.bundleCrt = true,
    this.vsInstallPath,
    this.architecture = 'x64',
    this.generator,
    this.generatorPath,
    this.extraDefines = const [],
  });
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

// ---------------------------------------------------------------------------
// Top-level build options
// ---------------------------------------------------------------------------

/// Top-level build options that apply across all platforms.
final class DcbBuildOptions {
  /// Whether to produce a Debug build.
  ///
  /// Defaults to `bool.fromEnvironment('dart.library.developer')`, which is
  /// `true` during `dart run` / `dart test` and `false` in AOT-compiled or
  /// release builds.
  ///
  /// Controls `CMAKE_BUILD_TYPE` (single-config generators) or `--config`
  /// (multi-config generators), and selects the debug or release CRT variant
  /// (`/MDd` vs `/MD`, `/MTd` vs `/MT`).
  final bool debug;

  const DcbBuildOptions({
    bool? debug,
  }) : debug = debug ??
            const bool.fromEnvironment('dart.library.developer');
}

// ---------------------------------------------------------------------------
// Exception
// ---------------------------------------------------------------------------

/// Thrown when a CMake invocation or artifact lookup fails in [DcbCMakeBuilder].
final class DcbCMakeException implements Exception {
  final String message;

  const DcbCMakeException(this.message);

  @override
  String toString() => 'DcbCMakeException: $message';
}

// ---------------------------------------------------------------------------
// Builder
// ---------------------------------------------------------------------------

/// Resolved Windows toolchain information (internal use).
final class _WindowsResolved {
  final String cmakePath;
  final List<String> configureArgs;
  final Map<String, String>? environment;

  const _WindowsResolved({
    required this.cmakePath,
    required this.configureArgs,
    this.environment,
  });
}

/// A reusable build-hook builder that drives CMake to compile a native library
/// and emits it as a bundled [CodeAsset].
///
/// The builder performs three CMake steps inside
/// `input.outputDirectory/dcb_build/`:
///
/// 1. `cmake -S <sourceDir> -B <buildDir> [generator] [defines]`
/// 2. `cmake --build <buildDir> --config <Release|Debug>`
/// 3. Locate the produced dynamic library and emit it as a [CodeAsset].
///
/// On Windows with [WindowsConfig.dynamicCrt] = `true` and
/// [WindowsConfig.bundleCrt] = `true`, the correct CRT runtime DLLs are copied
/// next to the output library after a successful build.
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

  /// Extra definitions passed verbatim to the CMake configure step,
  /// applied on all platforms. Platform-specific defines can also be set
  /// via [WindowsConfig.extraDefines].
  final List<String> extraDefines;

  /// Top-level build options (debug/release, etc.).
  final DcbBuildOptions buildOptions;

  const DcbCMakeBuilder({
    required this.config,
    required this.assetName,
    this.sourceDir,
    this.libName,
    this.extraDefines = const [],
    this.buildOptions = const DcbBuildOptions(),
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

    final buildType = buildOptions.debug ? 'Debug' : 'Release';

    // Resolve platform-specific settings.
    final String cmake;
    final configureArgs = <String>[];
    Map<String, String>? processEnvironment;

    switch (config) {
      case WindowsConfig cfg:
        final resolved = _resolveWindows(cfg, buildType);
        cmake = resolved.cmakePath;
        configureArgs.addAll(resolved.configureArgs);
        processEnvironment = resolved.environment;

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
    ], environment: processEnvironment);

    // 2. Build.
    await _runProcess(cmake, [
      '--build',
      buildDir.toFilePath(),
      '--config',
      buildType,
    ], environment: processEnvironment);

    // 3. Locate the produced dynamic library.
    final libFileName = targetOS.dylibFileName(libBaseName);
    final libFile = _locateArtifact(buildDir, libFileName, buildType);

    // 4. Bundle CRT DLLs if requested (Windows /MD only).
    if (config case WindowsConfig cfg) {
      if (cfg.dynamicCrt && cfg.bundleCrt) {
        _bundleWindowsCrt(cfg, libFile);
      }
    }

    // 5. Declare cache dependencies (CMakeLists + native sources).
    _declareDependencies(sourceRoot, output);

    // 6. Emit the bundled code asset.
    output.assets.code.add(
      CodeAsset(
        package: packageName,
        name: assetName,
        linkMode: DynamicLoadingBundled(),
        file: libFile.uri,
      ),
    );
  }

  // -------------------------------------------------------------------------
  // Windows resolution
  // -------------------------------------------------------------------------

  /// Resolves all Windows-specific paths and arguments.
  _WindowsResolved _resolveWindows(WindowsConfig cfg, String buildType) {
    // Resolve VS installation root.
    final vsRoot = cfg.vsInstallPath ?? _detectVsInstallPath();

    // Resolve CMake executable.
    final cmakePath = _resolveWindowsCmake(cfg.cmake, vsRoot);

    final args = <String>[];
    Map<String, String>? env;

    // Generator.
    switch (cfg.generator) {
      case CmakeGenerator.msbuild:
        final vsVersion = _vsGeneratorName(vsRoot);
        args.addAll(['-G', vsVersion]);
        args.addAll(['-A', cfg.architecture]);

      case CmakeGenerator.ninja:
        args.addAll(['-G', 'Ninja']);
        args.add('-DCMAKE_BUILD_TYPE=$buildType');
        // Ninja requires the MSVC environment (cl.exe on PATH).
        env = _initVcEnvironment(cfg, vsRoot);

      case null:
        // Let CMake auto-select (typically Visual Studio multi-config).
        // Still pass architecture for the VS generator.
        args.addAll(['-A', cfg.architecture]);
    }

    // CRT linkage.
    final crtValue = _msvcRuntimeLibrary(cfg.dynamicCrt, buildType);
    args.add('-DCMAKE_MSVC_RUNTIME_LIBRARY=$crtValue');

    // User-supplied extra defines.
    args.addAll(cfg.extraDefines);

    return _WindowsResolved(
      cmakePath: cmakePath,
      configureArgs: args,
      environment: env,
    );
  }

  /// Computes the `CMAKE_MSVC_RUNTIME_LIBRARY` value.
  static String _msvcRuntimeLibrary(bool dynamic, String buildType) {
    final isDebug = buildType == 'Debug';
    if (dynamic) {
      return isDebug ? 'MultiThreadedDebugDLL' : 'MultiThreadedDLL';
    }
    return isDebug ? 'MultiThreadedDebug' : 'MultiThreaded';
  }

  /// Determines the Visual Studio generator name (e.g. "Visual Studio 18 2026")
  /// from the installation root.
  String _vsGeneratorName(String? vsRoot) {
    if (vsRoot == null) {
      // Fall back: let CMake pick the default VS generator.
      return 'Visual Studio 18 2026';
    }
    // Probe the catalogVersion or infer from path segments.
    // Path pattern: ...\Microsoft Visual Studio\<major>\<edition>\
    final segments = vsRoot.replaceAll('/', '\\').split('\\');
    for (var i = 0; i < segments.length - 1; i++) {
      final ver = int.tryParse(segments[i]);
      if (ver != null && ver >= 15 && ver <= 99) {
        // Map internal version to year: 15→2017, 16→2019, 17→2022, 18→2026
        final year = switch (ver) {
          15 => '2017',
          16 => '2019',
          17 => '2022',
          18 => '2026',
          _ => '${2000 + ver}',
        };
        return 'Visual Studio $ver $year';
      }
    }
    return 'Visual Studio 18 2026';
  }

  // -------------------------------------------------------------------------
  // VS / Build Tools detection
  // -------------------------------------------------------------------------

  /// Detects the Visual Studio or Build Tools installation root.
  ///
  /// Priority: vswhere.exe → well-known path probe.
  String? _detectVsInstallPath() {
    // vswhere.exe is the official VS locator.
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
          final path = (result.stdout as String).trim();
          if (path.isNotEmpty && Directory(path).existsSync()) {
            _log('detected VS: $path');
            return path;
          }
        }
      } on ProcessException {
        // Fall through.
      }
    }

    // Well-known layout probe.
    const roots = [
      r'C:\Program Files\Microsoft Visual Studio',
      r'C:\Program Files (x86)\Microsoft Visual Studio',
    ];
    for (final root in roots) {
      final rootDir = Directory(root);
      if (!rootDir.existsSync()) continue;
      for (final versionDir in rootDir.listSync().whereType<Directory>()) {
        for (final editionDir in versionDir.listSync().whereType<Directory>()) {
          // Verify it looks like a VS install (has VC\Tools\MSVC).
          if (Directory('${editionDir.path}\\VC\\Tools\\MSVC').existsSync()) {
            _log('detected VS: ${editionDir.path}');
            return editionDir.path;
          }
        }
      }
    }
    return null;
  }

  // -------------------------------------------------------------------------
  // vcvars environment for Ninja
  // -------------------------------------------------------------------------

  /// Initializes the MSVC environment by invoking `vcvarsall.bat` and
  /// capturing the resulting environment variables.
  Map<String, String> _initVcEnvironment(WindowsConfig cfg, String? vsRoot) {
    final vcvarsall = _findVcvarsall(vsRoot);
    if (vcvarsall == null) {
      _log('WARNING: vcvarsall.bat not found; Ninja may fail to locate cl.exe');
      return Platform.environment;
    }

    final arch = cfg.architecture == 'arm64' ? 'x64_arm64' : 'x64';
    _log('initializing MSVC env: $vcvarsall $arch');

    // Run vcvarsall.bat and dump the resulting environment.
    final result = Process.runSync(
      'cmd.exe',
      ['/C', '"$vcvarsall" $arch >nul 2>&1 && set'],
    );
    if (result.exitCode != 0) {
      _log('WARNING: vcvarsall.bat failed (exit ${result.exitCode})');
      return Platform.environment;
    }

    final env = <String, String>{};
    for (final line in (result.stdout as String).split(RegExp(r'[\r\n]+'))) {
      final eq = line.indexOf('=');
      if (eq > 0) {
        env[line.substring(0, eq)] = line.substring(eq + 1);
      }
    }
    return env;
  }

  /// Locates `vcvarsall.bat` under the VS installation root.
  String? _findVcvarsall(String? vsRoot) {
    if (vsRoot != null) {
      final candidate = '$vsRoot\\VC\\Auxiliary\\Build\\vcvarsall.bat';
      if (File(candidate).existsSync()) return candidate;
    }
    return null;
  }

  // -------------------------------------------------------------------------
  // CRT bundling
  // -------------------------------------------------------------------------

  /// Copies the correct CRT runtime DLLs next to [outputDll].
  ///
  /// This ensures the built DLL loads the matching MSVCP140.dll version
  /// regardless of what the end-user's system has in System32.
  void _bundleWindowsCrt(WindowsConfig cfg, File outputDll) {
    final vsRoot = cfg.vsInstallPath ?? _detectVsInstallPath();
    if (vsRoot == null) {
      _log('WARNING: cannot bundle CRT — VS installation not found');
      return;
    }

    final redistBase = Directory('$vsRoot\\VC\\Redist\\MSVC');
    if (!redistBase.existsSync()) {
      _log('WARNING: cannot bundle CRT — Redist directory not found');
      return;
    }

    // Pick the latest version directory (e.g. "14.51.36231").
    final versionDirs = redistBase
        .listSync()
        .whereType<Directory>()
        .where((d) => RegExp(r'^\d+\.\d+').hasMatch(d.uri.pathSegments
            .where((s) => s.isNotEmpty)
            .last))
        .toList()
      ..sort((a, b) => a.path.compareTo(b.path));

    if (versionDirs.isEmpty) {
      _log('WARNING: cannot bundle CRT — no version directories found');
      return;
    }

    final arch = cfg.architecture == 'arm64' ? 'arm64' : 'x64';
    final crtDir = Directory(
      '${versionDirs.last.path}\\$arch\\Microsoft.VC145.CRT',
    );

    // Fallback: try VC143.CRT, VC140.CRT naming.
    final crtDirs = [
      crtDir,
      Directory('${versionDirs.last.path}\\$arch\\Microsoft.VC143.CRT'),
      Directory('${versionDirs.last.path}\\$arch\\Microsoft.VC140.CRT'),
    ];

    final sourceDir = crtDirs.firstWhere(
      (d) => d.existsSync(),
      orElse: () => crtDir, // Will log warning below.
    );

    if (!sourceDir.existsSync()) {
      _log('WARNING: cannot bundle CRT — CRT directory not found under '
          '${versionDirs.last.path}\\$arch\\');
      return;
    }

    const dllNames = [
      'MSVCP140.dll',
      'VCRUNTIME140.dll',
      'VCRUNTIME140_1.dll',
    ];

    final outDir = outputDll.parent;
    var copied = 0;
    for (final name in dllNames) {
      final src = File('${sourceDir.path}\\$name');
      if (src.existsSync()) {
        src.copySync('${outDir.path}\\$name');
        copied++;
      }
    }
    _log('bundled $copied CRT DLL(s) from ${sourceDir.path}');
  }

  // -------------------------------------------------------------------------
  // Process helpers
  // -------------------------------------------------------------------------

  /// Runs [executable] with [arguments], throwing [DcbCMakeException] on a
  /// non-zero exit code.
  Future<void> _runProcess(
    String executable,
    List<String> arguments, {
    Map<String, String>? environment,
  }) async {
    _log('$executable ${arguments.join(' ')}');
    final ProcessResult result;
    try {
      result = await Process.run(
        executable,
        arguments,
        environment: environment,
      );
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
  File _locateArtifact(Uri buildDir, String fileName, String buildType) {
    final candidates = <Uri>[
      buildDir.resolve('$buildType/').resolve(fileName),
      buildDir.resolve(fileName),
      // Fallback: check the other config in case of stale builds.
      buildDir
          .resolve(buildType == 'Debug' ? 'Release/' : 'Debug/')
          .resolve(fileName),
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
    for (final entity
        in rootDir.listSync(recursive: true, followLinks: false)) {
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

  // -------------------------------------------------------------------------
  // CMake resolution
  // -------------------------------------------------------------------------

  /// Resolves the CMake executable on Windows.
  ///
  /// When [configured] is the default bare `cmake`, first trusts `PATH`, then
  /// checks the VS installation at [vsRoot], then falls back to standalone
  /// installs and `vswhere.exe` discovery.
  String _resolveWindowsCmake(String configured, String? vsRoot) {
    if (configured != 'cmake') {
      return configured;
    }
    if (_isOnPath('cmake.exe')) {
      return configured;
    }

    // Try the resolved VS root first.
    if (vsRoot != null) {
      const cmakeRelative =
          r'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe';
      final candidate = '$vsRoot\\$cmakeRelative';
      if (File(candidate).existsSync()) {
        _log('resolved CMake: $candidate');
        return candidate;
      }
    }

    // Fallback: standalone installs and vswhere discovery.
    for (final candidate in _windowsCmakeCandidates()) {
      if (File(candidate).existsSync()) {
        _log('resolved CMake: $candidate');
        return candidate;
      }
    }
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
  /// first: standalone installs, then Visual Studio via `vswhere.exe` and a
  /// well-known layout probe.
  Iterable<String> _windowsCmakeCandidates() sync* {
    const cmakeRelative =
        r'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe';
    yield r'C:\Program Files\CMake\bin\cmake.exe';
    yield r'C:\Program Files (x86)\CMake\bin\cmake.exe';

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
        for (final editionDir
            in versionDir.listSync().whereType<Directory>()) {
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
