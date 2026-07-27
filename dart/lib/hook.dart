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

/// CMake build-system generator selection.
enum CmakeGenerator {
  /// Visual Studio multi-config generator (MSBuild).
  ///
  /// Produces `Release/` and `Debug/` subdirectories. This is the CMake
  /// default on Windows and requires no extra environment setup.
  /// Only applicable on Windows.
  msbuild,

  /// Ninja single-config generator.
  ///
  /// Faster incremental builds. On Windows, requires the MSVC environment
  /// to be initialized (the builder invokes `vcvarsall.bat` automatically).
  /// On Linux, works out of the box if `ninja` is on PATH.
  ninja,

  /// Unix Makefiles single-config generator.
  ///
  /// The CMake default on Linux when Ninja is not available. Slower than
  /// Ninja for incremental builds but requires no additional tools.
  /// Only applicable on Linux.
  makefiles,
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

/// Linux configuration for [DcbCMakeBuilder].
///
/// Linux uses single-config generators (Ninja or Unix Makefiles), so
/// `CMAKE_BUILD_TYPE` is set at configure time from [DcbBuildOptions.debug].
///
/// By default, libstdc++ is statically linked into the output `.so` to
/// produce a self-contained binary with no external C++ runtime dependency.
/// This mirrors Flutter's own Linux engine build strategy (clang compiler +
/// static libstdc++).
final class LinuxConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// CMake generator selection.
  ///
  /// - [CmakeGenerator.ninja]: faster builds, requires `ninja` on PATH.
  /// - [CmakeGenerator.makefiles]: default Unix Makefiles, always available.
  /// - `null`: let CMake auto-select (typically Unix Makefiles).
  final CmakeGenerator? generator;

  /// Explicit path to the generator executable.
  ///
  /// For [CmakeGenerator.ninja]: path to the `ninja` binary when it is not
  /// on PATH (e.g. `/opt/ninja/bin/ninja`).
  /// When `null`, resolved via PATH.
  final String? generatorPath;

  /// Path to the C++ compiler executable.
  ///
  /// Example: `/opt/llvm-18/bin/clang++` or `/usr/bin/g++-14`.
  ///
  /// When `null` (default), the system default compiler is used (usually
  /// `g++` or whatever `cc`/`c++` resolves to).
  ///
  /// Passed as `-DCMAKE_CXX_COMPILER=<path>`. CMake will auto-detect the
  /// corresponding C compiler and toolchain from the same directory.
  final String? compiler;

  /// Path to a CMake toolchain file for advanced cross-compilation or
  /// custom sysroot scenarios.
  ///
  /// Example: `/opt/toolchains/aarch64-linux-gnu.cmake`
  ///
  /// When specified, passed as `-DCMAKE_TOOLCHAIN_FILE=<path>`.
  /// This takes precedence over [compiler] — if both are set, the toolchain
  /// file is responsible for compiler selection and [compiler] is ignored.
  final String? toolchainFile;

  /// Whether to statically link libstdc++ into the output shared library.
  ///
  /// Defaults to `true`. Produces a self-contained `.so` that does not
  /// depend on the target system's `libstdc++.so.6` version. This avoids
  /// `GLIBCXX_3.4.XX not found` errors when deploying to systems with an
  /// older GCC runtime.
  ///
  /// Set to `false` to link dynamically against the system libstdc++
  /// (smaller binary, but requires a compatible runtime on the target).
  final bool staticLibStdCpp;

  /// Extra definitions passed verbatim to the CMake configure step, e.g.
  /// `['-DCMAKE_PREFIX_PATH=/opt/vcpkg']`.
  final List<String> extraDefines;

  const LinuxConfig({
    this.cmake = 'cmake',
    this.generator,
    this.generatorPath,
    this.compiler,
    this.toolchainFile,
    this.staticLibStdCpp = true,
    this.extraDefines = const [],
  });
}

/// Android configuration for [DcbCMakeBuilder].
///
/// Uses the NDK's CMake toolchain file for cross-compilation. The NDK ships
/// its own clang toolchain and libc++, so no separate compiler selection is
/// needed.
///
/// By default, libc++ is statically linked (`c++_static`). When multiple
/// native `.so` libraries coexist in the same APK, consider switching to
/// `c++_shared` to reduce APK size and avoid duplicated C++ runtime state.
/// Note: using `c++_shared` requires the shared library to be packaged into
/// the APK (AGP handles this automatically when using `externalNativeBuild`).
final class AndroidConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// Path to the Android NDK root directory.
  ///
  /// Example: `r'C:\Users\user\AppData\Local\Android\Sdk\ndk\29.0.14206865'`
  /// or `/opt/android-ndk-r29`.
  ///
  /// The builder derives the toolchain file from this path:
  /// `<ndkPath>/build/cmake/android.toolchain.cmake`.
  final String ndkPath;

  /// Target ABI (Android Binary Interface).
  ///
  /// Supported values: `'arm64-v8a'` (default), `'armeabi-v7a'`, `'x86_64'`,
  /// `'x86'`. Passed as `-DANDROID_ABI=<abi>`.
  final String abi;

  /// Minimum Android API level.
  ///
  /// Defaults to 21 (Android 5.0 Lollipop). Passed as
  /// `-DANDROID_PLATFORM=android-<level>`.
  ///
  /// Flutter's default `minSdkVersion` is typically 21 or 23. Match your
  /// app's `build.gradle` setting.
  final int androidPlatform;

  /// C++ STL linkage strategy.
  ///
  /// - `true` (default): static link libc++ (`-DANDROID_STL=c++_static`).
  ///   The output `.so` is self-contained with no external C++ dependency.
  /// - `false`: shared link libc++ (`-DANDROID_STL=c++_shared`).
  ///   Requires `libc++_shared.so` to be packaged in the APK. Use this when
  ///   multiple native libraries in the same app share C++ state.
  final bool staticStl;

  /// CMake generator selection.
  ///
  /// Defaults to [CmakeGenerator.ninja] (NDK bundles ninja).
  /// Set to `null` to let CMake auto-select.
  final CmakeGenerator? generator;

  /// Extra definitions passed verbatim to the CMake configure step.
  final List<String> extraDefines;

  const AndroidConfig({
    this.cmake = 'cmake',
    required this.ndkPath,
    this.abi = 'arm64-v8a',
    this.androidPlatform = 21,
    this.staticStl = true,
    this.generator = CmakeGenerator.ninja,
    this.extraDefines = const [],
  });
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
  /// When `null` (the default), the build type is inferred at runtime from
  /// `input.config.linkingEnabled`:
  /// - `linkingEnabled == true` → AOT / release → `Release`
  /// - `linkingEnabled == false` → JIT / debug → `Debug`
  ///
  /// Set explicitly to override auto-detection.
  ///
  /// Controls `CMAKE_BUILD_TYPE` (single-config generators) or `--config`
  /// (multi-config generators), and selects the debug or release CRT variant
  /// (`/MDd` vs `/MD`, `/MTd` vs `/MT`).
  final bool? debug;

  const DcbBuildOptions({this.debug});
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
  /// `@Native(assetId: 'package:<assetPackage>/<assetName>')`.
  final String assetName;

  /// The package namespace for the emitted code asset.
  ///
  /// Defaults to the building package's own name. Set this to
  /// `'dart_cpp_bridge'` when your downstream library embeds the runtime
  /// (via WHOLE_ARCHIVE) and wants `@Native(assetId: 'package:dart_cpp_bridge/...')`
  /// annotations to resolve to your combined library.
  final String? assetPackage;

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
    this.assetPackage,
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
    final effectiveAssetPackage = assetPackage ?? packageName;
    final libBaseName = libName ?? packageName;

    final sourceRoot = sourceDir == null
        ? input.packageRoot
        : input.packageRoot.resolve('$sourceDir/');
    final buildDir = input.outputDirectory.resolve('dcb_build/');
    Directory.fromUri(buildDir).createSync(recursive: true);

    // Infer debug/release: explicit override > linkingEnabled heuristic.
    // linkingEnabled is true for AOT (release) builds, false for JIT (debug).
    final isDebug = buildOptions.debug ?? !input.config.linkingEnabled;
    final buildType = isDebug ? 'Debug' : 'Release';

    // Resolve link mode from the hooks system preference.
    // Flutter sets this per-platform: iOS → static, others → dynamic.
    final linkMode = switch (input.config.code.linkModePreference) {
      LinkModePreference.static ||
      LinkModePreference.preferStatic => StaticLinking(),
      _ => DynamicLoadingBundled(),
    };
    final isDynamic = linkMode is DynamicLoadingBundled;

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

      case LinuxConfig cfg:
        cmake = cfg.cmake;
        configureArgs.addAll(_resolveLinuxArgs(cfg, buildType));
      case AndroidConfig cfg:
        cmake = cfg.cmake;
        configureArgs.addAll(_resolveAndroidArgs(cfg, buildType));
      case MacosConfig cfg:
        cmake = cfg.cmake;
        // Single-config generator: build type set at configure time.
        configureArgs.add('-DCMAKE_BUILD_TYPE=$buildType');
        configureArgs.add('-DCMAKE_POSITION_INDEPENDENT_CODE=ON');
        if (cfg.universal) {
          configureArgs.add('-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64');
        }
      case IosConfig():
        throw UnsupportedError('DcbCMakeBuilder: iOS is not supported yet.');
    }

    // 1. Configure.
    // Invalidate the build directory when configure arguments change
    // (e.g. switching between c++_static and c++_shared on Android).
    final allConfigureArgs = [
      '-DBUILD_SHARED_LIBS=${isDynamic ? 'ON' : 'OFF'}',
      ...configureArgs,
      ...extraDefines,
    ];
    _invalidateBuildOnConfigChange(buildDir, allConfigureArgs);

    await _runProcess(cmake, [
      '-S',
      sourceRoot.toFilePath(),
      '-B',
      buildDir.toFilePath(),
      ...allConfigureArgs,
    ], environment: processEnvironment);

    // 2. Build.
    await _runProcess(cmake, [
      '--build',
      buildDir.toFilePath(),
      '--config',
      buildType,
    ], environment: processEnvironment);

    // 3. Locate the produced library.
    final libFileName = targetOS.libraryFileName(libBaseName, linkMode);
    final libFile = _locateArtifact(buildDir, libFileName, buildType);

    // 4. Bundle CRT DLLs if requested (Windows /MD, dynamic only).
    if (config case WindowsConfig cfg) {
      if (isDynamic && cfg.dynamicCrt && cfg.bundleCrt) {
        _bundleWindowsCrt(cfg, libFile);
      }
    }

    // 5. Declare cache dependencies (CMakeLists + native sources).
    _declareDependencies(sourceRoot, output);

    // 6. Emit the code asset with the resolved link mode.
    output.assets.code.add(
      CodeAsset(
        package: effectiveAssetPackage,
        name: assetName,
        linkMode: linkMode,
        file: libFile.uri,
      ),
    );

    // 7. Bundle libc++_shared.so when using dynamic STL on Android.
    //    Without this, dlopen fails at runtime because the shared C++ runtime
    //    is not in the APK. AGP's externalNativeBuild handles this
    //    automatically, but Native Assets hooks bypass AGP's native build
    //    system, so we must register the dependency explicitly.
    if (config case AndroidConfig cfg when !cfg.staticStl) {
      _bundleAndroidSharedStl(cfg, effectiveAssetPackage, output);
    }
  }

  // -------------------------------------------------------------------------
  // Linux resolution
  // -------------------------------------------------------------------------

  /// Resolves Linux-specific configure arguments.
  List<String> _resolveLinuxArgs(LinuxConfig cfg, String buildType) {
    final args = <String>[];

    // Single-config generator: build type is set at configure time.
    args.add('-DCMAKE_BUILD_TYPE=$buildType');

    // All code must be position-independent since the final output is a
    // shared library (.so) that may incorporate static dependencies.
    args.add('-DCMAKE_POSITION_INDEPENDENT_CODE=ON');

    // Generator selection.
    switch (cfg.generator) {
      case CmakeGenerator.ninja:
        args.addAll(['-G', 'Ninja']);
        if (cfg.generatorPath != null) {
          args.add('-DCMAKE_MAKE_PROGRAM=${cfg.generatorPath}');
        }

      case CmakeGenerator.makefiles:
        args.addAll(['-G', 'Unix Makefiles']);
        if (cfg.generatorPath != null) {
          args.add('-DCMAKE_MAKE_PROGRAM=${cfg.generatorPath}');
        }

      case CmakeGenerator.msbuild:
        throw DcbCMakeException(
          'CmakeGenerator.msbuild is not valid on Linux. '
          'Use CmakeGenerator.ninja or CmakeGenerator.makefiles.',
        );

      case null:
        // Let CMake auto-select (typically Unix Makefiles).
        break;
    }

    // Toolchain file takes precedence over individual compiler setting.
    if (cfg.toolchainFile != null) {
      args.add('-DCMAKE_TOOLCHAIN_FILE=${cfg.toolchainFile}');
    } else if (cfg.compiler != null) {
      args.add('-DCMAKE_CXX_COMPILER=${cfg.compiler}');
    }

    // Static libstdc++ linkage for self-contained .so output.
    if (cfg.staticLibStdCpp) {
      args.add('-DCMAKE_CXX_STANDARD_LIBRARIES=-static-libstdc++');
    }

    // User-supplied extra defines.
    args.addAll(cfg.extraDefines);

    return args;
  }

  // -------------------------------------------------------------------------
  // Android resolution
  // -------------------------------------------------------------------------

  /// Resolves Android-specific configure arguments using the NDK toolchain.
  List<String> _resolveAndroidArgs(AndroidConfig cfg, String buildType) {
    final args = <String>[];

    // NDK toolchain file (the core of Android cross-compilation).
    final toolchainFile = '${cfg.ndkPath}/build/cmake/android.toolchain.cmake';
    if (!File(toolchainFile).existsSync()) {
      throw DcbCMakeException(
        'Android NDK toolchain file not found at: $toolchainFile\n'
        'Verify ndkPath points to a valid NDK installation.',
      );
    }
    args.add('-DCMAKE_TOOLCHAIN_FILE=$toolchainFile');

    // Target ABI and platform level.
    args.add('-DANDROID_ABI=${cfg.abi}');
    args.add('-DANDROID_PLATFORM=android-${cfg.androidPlatform}');

    // C++ STL linkage.
    args.add('-DANDROID_STL=${cfg.staticStl ? 'c++_static' : 'c++_shared'}');

    // Build type.
    args.add('-DCMAKE_BUILD_TYPE=$buildType');

    // Generator (Ninja is the standard for NDK builds).
    switch (cfg.generator) {
      case CmakeGenerator.ninja:
        args.addAll(['-G', 'Ninja']);

      case CmakeGenerator.makefiles:
        args.addAll(['-G', 'Unix Makefiles']);

      case CmakeGenerator.msbuild:
        throw DcbCMakeException(
          'CmakeGenerator.msbuild is not valid for Android cross-compilation.',
        );

      case null:
        break;
    }

    // User-supplied extra defines.
    args.addAll(cfg.extraDefines);

    return args;
  }

  /// Bundles `libc++_shared.so` from the NDK as an additional code asset.
  ///
  /// Required when [AndroidConfig.staticStl] is `false` (dynamic STL linkage).
  /// The NDK does not automatically package the shared C++ runtime into the
  /// APK when building via Native Assets hooks (unlike AGP's
  /// `externalNativeBuild`), so we register it explicitly.
  void _bundleAndroidSharedStl(
    AndroidConfig cfg,
    String packageName,
    BuildOutputBuilder output,
  ) {
    // Map ABI to the NDK's target triple directory name.
    final abiDir = switch (cfg.abi) {
      'arm64-v8a' => 'aarch64-linux-android',
      'armeabi-v7a' => 'arm-linux-androideabi',
      'x86_64' => 'x86_64-linux-android',
      'x86' => 'i686-linux-android',
      _ => throw DcbCMakeException(
        'Unknown Android ABI: ${cfg.abi}. '
        'Supported: arm64-v8a, armeabi-v7a, x86_64, x86.',
      ),
    };

    // Detect the NDK prebuilt host tag.
    final hostTag = _detectNdkHostTag();

    final sharedStl = File(
      '${cfg.ndkPath}/toolchains/llvm/prebuilt/$hostTag'
      '/sysroot/usr/lib/$abiDir/libc++_shared.so',
    );
    if (!sharedStl.existsSync()) {
      throw DcbCMakeException(
        'libc++_shared.so not found at: ${sharedStl.path}\n'
        'Verify ndkPath and abi are correct.',
      );
    }

    output.assets.code.add(
      CodeAsset(
        package: packageName,
        name: 'libc++_shared.so',
        linkMode: DynamicLoadingBundled(),
        file: sharedStl.uri,
      ),
    );
  }

  /// Detects the NDK prebuilt host tag (e.g. `windows-x86_64`, `linux-x86_64`).
  String _detectNdkHostTag() {
    if (Platform.isWindows) return 'windows-x86_64';
    if (Platform.isMacOS) return 'darwin-x86_64';
    return 'linux-x86_64';
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

      case CmakeGenerator.makefiles:
        throw DcbCMakeException(
          'CmakeGenerator.makefiles is not valid on Windows. '
          'Use CmakeGenerator.msbuild or CmakeGenerator.ninja.',
        );

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
    final result = Process.runSync('cmd.exe', [
      '/C',
      '"$vcvarsall" $arch >nul 2>&1 && set',
    ]);
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
    final versionDirs =
        redistBase
            .listSync()
            .whereType<Directory>()
            .where(
              (d) => RegExp(
                r'^\d+\.\d+',
              ).hasMatch(d.uri.pathSegments.where((s) => s.isNotEmpty).last),
            )
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
      _log(
        'WARNING: cannot bundle CRT — CRT directory not found under '
        '${versionDirs.last.path}\\$arch\\',
      );
      return;
    }

    const dllNames = ['MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll'];

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

  /// Checks a stamp file in [buildDir]; if the recorded configure arguments
  /// differ from [currentArgs], deletes the build directory to force a clean
  /// reconfigure. This prevents stale CMake caches (e.g. switching Android STL
  /// from c++_static to c++_shared) from producing broken builds.
  void _invalidateBuildOnConfigChange(Uri buildDir, List<String> currentArgs) {
    final stampFile = File.fromUri(buildDir.resolve('.dcb_configure_args'));
    final currentStamp = currentArgs.join('\n');
    if (stampFile.existsSync()) {
      final previousStamp = stampFile.readAsStringSync();
      if (previousStamp == currentStamp) {
        return; // Config unchanged, reuse build directory.
      }
      // Config changed — wipe the stale build directory.
      Directory.fromUri(buildDir).deleteSync(recursive: true);
    }
    Directory.fromUri(buildDir).createSync(recursive: true);
    stampFile.writeAsStringSync(currentStamp);
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
      '.c',
      '.cc',
      '.cpp',
      '.cxx',
      '.c++',
      '.h',
      '.hh',
      '.hpp',
      '.hxx',
      '.h++',
      '.m',
      '.mm',
      '.cmake',
    };
    for (final entity in rootDir.listSync(
      recursive: true,
      followLinks: false,
    )) {
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
