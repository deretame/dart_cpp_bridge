/// Build-hook support for **dart_cpp_bridge** Native Assets.
///
/// Provides a platform-configuration sealed hierarchy ([DcbPlatformConfig]) and
/// a reusable CMake-driven builder ([DcbCMakeBuilder]) that downstream packages
/// use from their `hook/build.dart` to compile a native library and emit it as
/// a bundled [CodeAsset], consumable at runtime via
/// `@Native(assetId: 'package:<package>/<assetName>')`.
///
/// Supported platforms: Windows, Linux, macOS, Android, iOS.
///
/// This library is intentionally separate from `package:dart_cpp_bridge`
/// (the runtime FFI front-end): it depends on `package:hooks` /
/// `package:code_assets`, which are only needed inside a build hook.
library;

import 'dart:convert';
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

/// Windows C++ compiler selection.
enum WindowsCompiler {
  /// Microsoft Visual C++ (`cl.exe`), the default.
  msvc,

  /// LLVM `clang-cl` — the MSVC-compatible clang driver.
  ///
  /// Requires an LLVM installation (e.g. from https://llvm.org or the Visual
  /// Studio "C++ Clang tools for Windows" component) and the Visual Studio
  /// C++ workload (the Windows SDK headers/libs and the MSVC STL are still
  /// consumed from the VS installation).
  ///
  /// Build-mode notes:
  /// - With [CmakeGenerator.ninja] (the recommended mode, also the default
  ///   when [WindowsConfig.generator] is `null`), the builder initializes the
  ///   MSVC environment via `vcvarsall.bat` **first**, then locates
  ///   clang-cl: an explicit [WindowsConfig.clangClPath] wins, then a
  ///   VS-bundled clang-cl (visible on the vcvars PATH when the VS Clang
  ///   component is installed), then `PATH` / LLVM installs / the LLVM
  ///   registry key. The resolved compiler is passed via
  ///   `-DCMAKE_C(XX)_COMPILER=<clang-cl>`, so clang-cl does not need to be
  ///   on PATH beforehand.
  /// - With [CmakeGenerator.msbuild], the builder passes `-T clangcl` to
  ///   CMake, which selects the clang-cl toolset inside Visual Studio.
  clangCl,

  /// MSYS2 ucrt64 `clang` — the GNU/MinGW-style clang toolchain
  /// (`x86_64-w64-windows-gnu` target), e.g. from the
  /// `mingw-w64-ucrt-x86_64-clang` package.
  ///
  /// This is a completely different ABI from MSVC: no `vcvarsall.bat`
  /// environment is needed (the toolchain carries its own headers and
  /// libraries), and the produced DLL links against the MSYS2 runtime
  /// (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` from
  /// `<msys2>\ucrt64\bin`) instead of the MSVC CRT. Make sure those DLLs are
  /// on `PATH` (or bundled) at runtime.
  ///
  /// Build-mode notes:
  /// - Only [CmakeGenerator.ninja] is supported (the default when
  ///   [WindowsConfig.generator] is `null`). MSBuild / Makefiles are not
  ///   usable with a GNU-style toolchain.
  /// - The builder resolves the MSYS2 root via [WindowsConfig.msys2Path],
  ///   then the `MSYS2_ROOT` environment variable, then well-known install
  ///   locations, and passes `-DCMAKE_C(XX)_COMPILER=<msys2>\ucrt64\bin\
  ///   clang(++).exe`. The `<msys2>\ucrt64\bin` directory is added to `PATH`
  ///   for the CMake process.
  /// - With [WindowsConfig.staticRuntime] (default `true`) the C++ runtime
  ///   (libgcc / libstdc++ / winpthread) is statically linked into the DLL,
  ///   producing a self-contained DLL with no MSYS2 runtime DLL dependency.
  /// - `dynamicCrt` / `bundleCrt` do not apply to the MSVC CRT (no MSVC CRT
  ///   dependency).
  msys2Clang,

  /// MSYS2 ucrt64 `gcc`/`g++` — the GNU/MinGW-style GCC toolchain
  /// (`x86_64-w64-windows-gnu` target), e.g. from the
  /// `mingw-w64-ucrt-x86_64-gcc` package.
  ///
  /// Same model as [msys2Clang] (GNU ABI, self-contained toolchain, Ninja
  /// only, [WindowsConfig.msys2Path] / `MSYS2_ROOT` / well-known install
  /// locations for resolution, `-DCMAKE_C(XX)_COMPILER=<msys2>\ucrt64\bin\
  /// gcc(++).exe`), and statically links the C++ runtime by default via
  /// [WindowsConfig.staticRuntime].
  msys2Gcc,
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

  /// Optional target architecture override passed to CMake (`-A`).
  ///
  /// Supported values: `'x64'`, `'arm64'`. When omitted, the builder follows
  /// `input.config.code.targetArchitecture` instead of silently defaulting an
  /// ARM64 hook request to x64. Reading [architecture] still returns the
  /// effective value for compatibility.
  final String? _architectureOverride;

  /// Effective Windows architecture (`'x64'` or `'arm64'`).
  String get architecture => _architectureOverride ?? 'x64';

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

  /// C++ compiler selection.
  ///
  /// Defaults to [WindowsCompiler.msvc]. Set to [WindowsCompiler.clangCl] to
  /// build with LLVM's MSVC-compatible `clang-cl` driver instead of `cl.exe`.
  /// See [WindowsCompiler.clangCl] for the per-generator behavior.
  final WindowsCompiler compiler;

  /// Explicit path to `clang-cl.exe` (e.g. `r'C:\LLVM\bin\clang-cl.exe'`).
  ///
  /// Only used when [compiler] is [WindowsCompiler.clangCl]. When `null`
  /// (default), the builder locates clang-cl automatically, after
  /// initializing the MSVC environment: a VS-bundled clang-cl (from the
  /// "C++ Clang tools for Windows" component, visible on the vcvars PATH)
  /// first, then `PATH`, then `C:\Program Files\LLVM\bin\clang-cl.exe` /
  /// `C:\Program Files (x86)\LLVM\bin\clang-cl.exe`, then the LLVM registry
  /// key. With [CmakeGenerator.ninja] the resolved directory is added to
  /// `PATH` for the CMake process, so clang-cl does not need to be on PATH
  /// globally.
  final String? clangClPath;

  /// MSYS2 installation root (e.g. `r'D:\msys2'`), used only when
  /// [compiler] is [WindowsCompiler.msys2Clang] or
  /// [WindowsCompiler.msys2Gcc].
  ///
  /// When `null` (default), the builder resolves the root from the
  /// `MSYS2_ROOT` environment variable, then probes well-known install
  /// locations (`C:\msys64`, `C:\msys2`, `D:\msys2`). The toolchain is
  /// expected at `<root>\ucrt64\bin\clang(++).exe` (msys2Clang) or
  /// `<root>\ucrt64\bin\gcc(++).exe` (msys2Gcc).
  final String? msys2Path;

  /// Whether to statically link the GNU C++ runtime into the output library
  /// for MSYS2 toolchains ([WindowsCompiler.msys2Clang] /
  /// [WindowsCompiler.msys2Gcc]).
  ///
  /// Defaults to `true`: `-static-libgcc -static-libstdc++` plus static
  /// winpthread are passed to the compiler/linker, producing a self-contained
  /// DLL that only depends on the system UCRT — no MSYS2 runtime DLLs
  /// (`libgcc_s_seh-1.dll` / `libstdc++-6.dll` / `libwinpthread-1.dll`) are
  /// needed at runtime, so no `PATH` setup is required.
  ///
  /// Set to `false` to link the MSYS2 runtime dynamically; the DLL then needs
  /// those runtime DLLs at load time (keep `<msys2>\ucrt64\bin` on `PATH` or
  /// bundle them next to the output — see [bundleCrt]).
  final bool staticRuntime;

  const WindowsConfig({
    this.cmake = 'cmake',
    this.dynamicCrt = true,
    this.bundleCrt = true,
    this.vsInstallPath,
    String? architecture,
    this.generator,
    this.generatorPath,
    this.extraDefines = const [],
    this.compiler = WindowsCompiler.msvc,
    this.clangClPath,
    this.msys2Path,
    this.staticRuntime = true,
  }) : _architectureOverride = architecture;
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
  /// Supported values: `'arm64-v8a'`, `'armeabi-v7a'`, `'x86_64'`, `'x86'`.
  /// Passed as `-DANDROID_ABI=<abi>`.
  ///
  /// When `null` (default), the ABI is derived automatically from
  /// `input.config.code.targetArchitecture`:
  /// - `arm64` → `'arm64-v8a'`
  /// - `arm` → `'armeabi-v7a'`
  /// - `x64` → `'x86_64'`
  /// - `ia32` → `'x86'`
  final String? abi;

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
    this.abi,
    this.androidPlatform = 21,
    this.staticStl = true,
    this.generator = CmakeGenerator.ninja,
    this.extraDefines = const [],
  });
}

/// macOS configuration for [DcbCMakeBuilder].
///
/// macOS uses single-config generators (Ninja or Unix Makefiles), so
/// `CMAKE_BUILD_TYPE` is set at configure time from [DcbBuildOptions.debug].
///
/// The default generator is Unix Makefiles (CMake's default on macOS).
/// AppleClang is the standard compiler; custom compiler selection is rarely
/// needed but supported via [compiler].
final class MacosConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// CMake generator selection.
  ///
  /// - [CmakeGenerator.ninja]: faster builds, requires `ninja` on PATH.
  /// - [CmakeGenerator.makefiles]: default Unix Makefiles, always available.
  /// - `null`: let CMake auto-select (typically Unix Makefiles on macOS).
  final CmakeGenerator? generator;

  /// Explicit path to the generator executable.
  ///
  /// For [CmakeGenerator.ninja]: path to the `ninja` binary when it is not
  /// on PATH (e.g. `/opt/homebrew/bin/ninja`).
  /// When `null`, resolved via PATH.
  final String? generatorPath;

  /// Path to the C++ compiler executable.
  ///
  /// Example: `/opt/homebrew/opt/llvm/bin/clang++` for a Homebrew LLVM.
  ///
  /// When `null` (default), the system AppleClang is used.
  ///
  /// Passed as `-DCMAKE_CXX_COMPILER=<path>`.
  final String? compiler;

  /// Minimum macOS deployment target (e.g. `'13.0'`).
  ///
  /// Passed as `-DCMAKE_OSX_DEPLOYMENT_TARGET=<value>`.
  ///
  /// When `null` (default), CMake uses the SDK's default minimum.
  /// Set this when distributing to users on older macOS versions.
  final String? deploymentTarget;

  /// Whether to build a universal (arm64 + x86_64) binary.
  ///
  /// When `true`, passes `-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64`.
  /// Defaults to `false` (build only the hooks-provided target architecture).
  ///
  /// With Flutter Native Assets / code_assets, keep this `false`: the hooks
  /// system invokes the builder once per target architecture and merges the
  /// per-architecture dylibs into a universal binary itself. Enabling
  /// `universal` there produces a fat dylib per invocation and breaks the
  /// final `lipo -create` merge.
  final bool universal;

  /// Extra definitions passed verbatim to the CMake configure step, e.g.
  /// `['-DCMAKE_PREFIX_PATH=/opt/homebrew']`.
  final List<String> extraDefines;

  const MacosConfig({
    this.cmake = 'cmake',
    this.generator,
    this.generatorPath,
    this.compiler,
    this.deploymentTarget,
    this.universal = false,
    this.extraDefines = const [],
  });
}

/// iOS configuration for [DcbCMakeBuilder].
///
/// iOS requires static linking (dynamic loading is forbidden by the OS).
/// The builder reads `input.config.code.iOS.targetSdk` to distinguish
/// device (`iphoneos`) from simulator (`iphonesimulator`) builds, and
/// `input.config.code.iOS.targetVersion` for the deployment target.
///
/// The Xcode toolchain (AppleClang + iOS SDK) is used automatically via
/// `-DCMAKE_SYSTEM_NAME=iOS`. No custom compiler selection is needed.
final class IosConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// CMake generator selection.
  ///
  /// - [CmakeGenerator.ninja]: faster builds, requires `ninja` on PATH.
  /// - [CmakeGenerator.makefiles]: Unix Makefiles, always available.
  /// - `null`: let CMake auto-select.
  final CmakeGenerator? generator;

  /// Explicit path to the generator executable.
  ///
  /// For [CmakeGenerator.ninja]: path to the `ninja` binary when it is not
  /// on PATH (e.g. `/opt/homebrew/bin/ninja`).
  /// When `null`, resolved via PATH.
  final String? generatorPath;

  /// Xcode developer directory path.
  ///
  /// When `null` (default), uses the system default from `xcode-select -p`.
  /// Passed as `-DCMAKE_DEVELOPER_DIRECTORY=<path>` (via
  /// `DEVELOPER_DIR` environment variable).
  final String? developerDir;

  /// Minimum iOS deployment target override (e.g. `'15.0'`).
  ///
  /// When `null` (default), the value is read from
  /// `input.config.code.iOS.targetVersion` provided by the hooks system.
  /// Set this only to override Flutter's default minimum.
  final String? deploymentTarget;

  /// Extra definitions passed verbatim to the CMake configure step.
  final List<String> extraDefines;

  const IosConfig({
    this.cmake = 'cmake',
    this.generator,
    this.generatorPath,
    this.developerDir,
    this.deploymentTarget,
    this.extraDefines = const [],
  });
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

  /// Whether to enable parallel (multi-threaded) compilation.
  ///
  /// Defaults to `true`. When enabled, passes `--parallel` to `cmake --build`,
  /// allowing CMake to compile multiple source files concurrently.
  final bool parallel;

  /// Whether to generate and copy `compile_commands.json` to the package root.
  ///
  /// Defaults to `true`. When enabled:
  /// - `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` is passed to the CMake configure
  ///   step (for generators that support it, e.g. Ninja / Makefiles), unless
  ///   [DcbCMakeBuilder.useDefaultCmakeArgs] is `false`.
  /// - After a successful build, the generated `compile_commands.json` is
  ///   copied to [compileCommandsPath] under the package root if it exists.
  ///
  /// This makes LSP / clangd tooling work out of the box without manual
  /// symlinks. Set to `false` to disable generation and copying.
  final bool copyCompileCommands;

  /// Relative path under the package root where `compile_commands.json` is
  /// copied.
  ///
  /// Defaults to `'compile_commands.json'` (i.e. directly next to
  /// `pubspec.yaml`). Use a relative path like `'build/compile_commands.json'`
  /// or `'.vscode/compile_commands.json'` to keep the package root tidy.
  ///
  /// Ignored when [copyCompileCommands] is `false`.
  final String compileCommandsPath;

  const DcbBuildOptions({
    this.debug,
    this.parallel = true,
    this.copyCompileCommands = true,
    this.compileCommandsPath = 'compile_commands.json',
  });
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
  final String architecture;

  const _WindowsResolved({
    required this.cmakePath,
    required this.configureArgs,
    required this.architecture,
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

  /// Whether to inject the builder's default CMake configure arguments.
  ///
  /// Defaults to `true`, which preserves the normal platform-aware behavior:
  /// generator, architecture, toolchain, build type, runtime linkage,
  /// `BUILD_SHARED_LIBS`, and (when enabled) compile-command export arguments
  /// are generated by the builder.
  ///
  /// Set to `false` when the project's `CMakeLists.txt` or an explicitly
  /// supplied [extraDefines] list owns all configure-time decisions. The
  /// required `-S` / `-B` arguments and the separate `cmake --build` step are
  /// still used. `extraDefines` and the platform config's `extraDefines` are
  /// still passed through.
  final bool useDefaultCmakeArgs;

  /// Top-level build options (debug/release, etc.).
  final DcbBuildOptions buildOptions;

  const DcbCMakeBuilder({
    required this.config,
    required this.assetName,
    this.assetPackage,
    this.sourceDir,
    this.libName,
    this.extraDefines = const [],
    this.useDefaultCmakeArgs = true,
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
    String? windowsArchitecture;

    switch (config) {
      case WindowsConfig cfg:
        final resolved = _resolveWindows(
          cfg,
          buildType,
          input.config.code.targetArchitecture,
        );
        cmake = resolved.cmakePath;
        configureArgs.addAll(resolved.configureArgs);
        processEnvironment = resolved.environment;
        windowsArchitecture = resolved.architecture;

      case LinuxConfig cfg:
        cmake = cfg.cmake;
        configureArgs.addAll(_resolveLinuxArgs(cfg, buildType));
      case AndroidConfig cfg:
        final abi = _resolveAndroidAbi(
          input.config.code.targetArchitecture,
          cfg.abi,
        );
        cmake = _resolveAndroidCmake(cfg);
        configureArgs.addAll(_resolveAndroidArgs(cfg, buildType, abi));
        // Ninja generator needs ninja.exe on PATH; inject VS Ninja dir.
        if (cfg.generator == CmakeGenerator.ninja && Platform.isWindows) {
          processEnvironment = _ensureNinjaOnPath(cmake);
        }
      case MacosConfig cfg:
        cmake = cfg.cmake;
        configureArgs.addAll(
          _resolveMacosArgs(
            cfg,
            buildType,
            input.config.code.targetArchitecture,
          ),
        );
      case IosConfig cfg:
        cmake = cfg.cmake;
        configureArgs.addAll(
          _resolveIosArgs(cfg, buildType, input.config.code),
        );
        // DEVELOPER_DIR environment variable for xcrun.
        if (cfg.developerDir != null) {
          processEnvironment = Map<String, String>.from(Platform.environment);
          processEnvironment['DEVELOPER_DIR'] = cfg.developerDir!;
        }
    }

    // Resolve package_config rootUri with Dart's URI implementation. CMake's
    // string slicing leaves percent escapes in filesystem paths, which breaks
    // projects installed under spaces or non-ASCII directories. Passing the
    // decoded absolute path also lets dcb_find_package.cmake use its fast path.
    final dcbPackagePath = _resolveDartCppBridgePackagePath(input.packageRoot);
    final dcbPackageDefine = dcbPackagePath == null
        ? const <String>[]
        : ['-DDCB_PKG_PATH=$dcbPackagePath'];

    // 1. Configure.
    // Invalidate the build directory when configure arguments change
    // (e.g. switching between c++_static and c++_shared on Android).
    final allConfigureArgs = [
      if (useDefaultCmakeArgs) ...[
        '-DBUILD_SHARED_LIBS=${isDynamic ? 'ON' : 'OFF'}',
        if (buildOptions.copyCompileCommands)
          '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        ...configureArgs,
      ] else ...[
        ..._platformExtraDefines(config),
      ],
      ...dcbPackageDefine,
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
      if (!_isSingleConfigGenerator(config)) ...['--config', buildType],
      if (buildOptions.parallel) '--parallel',
    ], environment: processEnvironment);

    // 3. Copy compile_commands.json for LSP / clangd.
    if (buildOptions.copyCompileCommands) {
      _copyCompileCommandsIfPresent(
        buildDir: buildDir,
        buildType: buildType,
        packageRoot: input.packageRoot,
        relativeDest: buildOptions.compileCommandsPath,
      );
    }

    // 4. Locate the produced library.
    final libFileName = targetOS.libraryFileName(libBaseName, linkMode);
    final libFile = _locateArtifact(buildDir, libFileName, buildType);

    // 4. Bundle runtime DLLs if requested (Windows /MD, dynamic only).
    //    - MSVC / clang-cl: the MSVC CRT DLLs (MSVCP140.dll etc.).
    //    - MSYS2 toolchains with staticRuntime=false: the MSYS2 runtime DLLs
    //      (libgcc_s_seh-1.dll etc.). With staticRuntime=true (default) the
    //      runtime is statically linked, so nothing needs bundling.
    final bundledWindowsRuntime = <File>[];
    if (config case WindowsConfig cfg) {
      final isMsys2 =
          cfg.compiler == WindowsCompiler.msys2Clang ||
          cfg.compiler == WindowsCompiler.msys2Gcc;
      if (isMsys2) {
        if (isDynamic && !cfg.staticRuntime && cfg.bundleCrt) {
          bundledWindowsRuntime.addAll(_bundleMsys2Runtime(cfg, libFile));
        }
      } else if (isDynamic && cfg.dynamicCrt && cfg.bundleCrt) {
        bundledWindowsRuntime.addAll(
          _bundleWindowsCrt(cfg, libFile, windowsArchitecture!),
        );
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

    // A DLL copied next to the main library is not discovered by the Native
    // Assets toolchain automatically. Register every bundled runtime DLL as
    // its own code asset so Flutter includes it in the final application.
    for (final runtimeDll in bundledWindowsRuntime) {
      output.assets.code.add(
        CodeAsset(
          package: effectiveAssetPackage,
          name: runtimeDll.uri.pathSegments.last,
          linkMode: DynamicLoadingBundled(),
          file: runtimeDll.uri,
        ),
      );
    }

    // 7. Bundle libc++_shared.so when using dynamic STL on Android.
    //    Without this, dlopen fails at runtime because the shared C++ runtime
    //    is not in the APK. AGP's externalNativeBuild handles this
    //    automatically, but Native Assets hooks bypass AGP's native build
    //    system, so we must register the dependency explicitly.
    if (config case AndroidConfig cfg when !cfg.staticStl) {
      final abi = _resolveAndroidAbi(
        input.config.code.targetArchitecture,
        cfg.abi,
      );
      _bundleAndroidSharedStl(cfg, effectiveAssetPackage, output, abi);
    }
  }

  /// Whether the configured CMake generator is single-config.
  ///
  /// On Windows, only [CmakeGenerator.ninja] is single-config (including the
  /// implicit Ninja default used when [WindowsConfig.compiler] is
  /// [WindowsCompiler.clangCl] / [WindowsCompiler.msys2Clang] /
  /// [WindowsCompiler.msys2Gcc] and [WindowsConfig.generator] is `null`); the
  /// Visual Studio / MSBuild generator is multi-config. All other supported
  /// platforms (Linux, macOS, iOS, Android) use single-config generators.
  bool _isSingleConfigGenerator(DcbPlatformConfig cfg) {
    if (cfg is WindowsConfig) {
      return cfg.generator == CmakeGenerator.ninja ||
          (cfg.generator == null &&
              (cfg.compiler == WindowsCompiler.clangCl ||
                  cfg.compiler == WindowsCompiler.msys2Clang ||
                  cfg.compiler == WindowsCompiler.msys2Gcc));
    }
    return true;
  }

  /// Returns the decoded local path of the dart_cpp_bridge dependency from
  /// [packageRoot]'s package_config.json, or null when this package does not
  /// use the bridge's CMake integration.
  static String? _resolveDartCppBridgePackagePath(Uri packageRoot) {
    final packageConfig = File.fromUri(
      packageRoot.resolve('.dart_tool/package_config.json'),
    );
    if (!packageConfig.existsSync()) return null;
    try {
      final json = jsonDecode(packageConfig.readAsStringSync());
      if (json is! Map<String, dynamic>) return null;
      final packages = json['packages'];
      if (packages is! List) return null;
      for (final entry in packages) {
        if (entry is! Map<String, dynamic> ||
            entry['name'] != 'dart_cpp_bridge') {
          continue;
        }
        final rawRootUri = entry['rootUri'];
        if (rawRootUri is! String) return null;
        final rootUri = Uri.parse(rawRootUri);
        final resolved = rootUri.scheme == 'file'
            ? rootUri
            : packageConfig.uri.resolveUri(rootUri);
        if (resolved.scheme != 'file') return null;
        final path = resolved.toFilePath(windows: Platform.isWindows);
        return Directory(path).existsSync() ? path : null;
      }
    } on FormatException {
      return null;
    } on FileSystemException {
      return null;
    }
    return null;
  }

  /// Returns only the explicit platform-level definitions.
  ///
  /// The platform resolvers also add generated defaults, so these values are
  /// kept separately when [useDefaultCmakeArgs] is `false`.
  List<String> _platformExtraDefines(DcbPlatformConfig cfg) => switch (cfg) {
    WindowsConfig(:final extraDefines) => extraDefines,
    LinuxConfig(:final extraDefines) => extraDefines,
    AndroidConfig(:final extraDefines) => extraDefines,
    MacosConfig(:final extraDefines) => extraDefines,
    IosConfig(:final extraDefines) => extraDefines,
  };

  // -------------------------------------------------------------------------
  // macOS resolution
  // -------------------------------------------------------------------------

  /// Resolves macOS-specific configure arguments.
  List<String> _resolveMacosArgs(
    MacosConfig cfg,
    String buildType,
    Architecture targetArchitecture,
  ) {
    final args = <String>[];

    // Single-config generator: build type is set at configure time.
    args.add('-DCMAKE_BUILD_TYPE=$buildType');

    // All code must be position-independent since the final output is a
    // shared library (.dylib) that may incorporate static dependencies.
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
          'CmakeGenerator.msbuild is not valid on macOS. '
          'Use CmakeGenerator.ninja or CmakeGenerator.makefiles.',
        );

      case null:
        // Let CMake auto-select (typically Unix Makefiles on macOS).
        break;
    }

    // Custom compiler (rarely needed on macOS).
    if (cfg.compiler != null) {
      args.add('-DCMAKE_CXX_COMPILER=${cfg.compiler}');
    }

    // Deployment target.
    if (cfg.deploymentTarget != null) {
      args.add('-DCMAKE_OSX_DEPLOYMENT_TARGET=${cfg.deploymentTarget}');
    }

    // Architecture: explicit universal builds combine both slices; otherwise
    // build exactly the architecture requested by the hooks system, so each
    // per-architecture invocation produces a distinct slice for lipo.
    if (cfg.universal) {
      args.add('-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64');
    } else {
      final arch = switch (targetArchitecture) {
        Architecture.arm64 => 'arm64',
        Architecture.x64 => 'x86_64',
        final a => throw DcbCMakeException(
            'Unsupported macOS architecture: $a. '
            'macOS supports arm64 and x86_64.',
          ),
      };
      args.add('-DCMAKE_OSX_ARCHITECTURES=$arch');
    }

    // User-supplied extra defines.
    args.addAll(cfg.extraDefines);

    return args;
  }

  // -------------------------------------------------------------------------
  // iOS resolution
  // -------------------------------------------------------------------------

  /// Resolves iOS-specific configure arguments.
  ///
  /// Uses `codeConfig.iOS.targetSdk` to select the sysroot (device vs
  /// simulator) and `codeConfig.iOS.targetVersion` for the deployment target.
  List<String> _resolveIosArgs(
    IosConfig cfg,
    String buildType,
    CodeConfig codeConfig,
  ) {
    final args = <String>[];

    // Cross-compilation: tell CMake we're targeting iOS.
    args.add('-DCMAKE_SYSTEM_NAME=iOS');

    // Single-config generator: build type is set at configure time.
    args.add('-DCMAKE_BUILD_TYPE=$buildType');

    // Position-independent code (required for static libs linked into dylibs).
    args.add('-DCMAKE_POSITION_INDEPENDENT_CODE=ON');

    // Sysroot: device vs simulator.
    final iosConfig = codeConfig.iOS;
    final sysroot = switch (iosConfig.targetSdk) {
      IOSSdk.iPhoneOS => 'iphoneos',
      IOSSdk.iPhoneSimulator => 'iphonesimulator',
      _ => 'iphoneos',
    };
    args.add('-DCMAKE_OSX_SYSROOT=$sysroot');

    // Architecture: derived from the hooks target architecture.
    final arch = switch (codeConfig.targetArchitecture) {
      Architecture.arm64 => 'arm64',
      Architecture.x64 => 'x86_64',
      final a => throw DcbCMakeException(
          'Unsupported iOS architecture: $a. '
          'iOS supports arm64 (device/simulator) and x86_64 (simulator).',
        ),
    };
    args.add('-DCMAKE_OSX_ARCHITECTURES=$arch');

    // Deployment target: explicit override > hooks-provided version.
    // Floor at 14.0: the runtime uses C++20 std::atomic::wait/notify
    // which requires iOS 14.0+ (simulator) / iOS 14.0+ (device).
    final rawVersion = cfg.deploymentTarget ??
        '${iosConfig.targetVersion}.0';
    final deploymentTarget = _enforceMinimumIosVersion(rawVersion, 14);
    args.add('-DCMAKE_OSX_DEPLOYMENT_TARGET=$deploymentTarget');

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
          'CmakeGenerator.msbuild is not valid on iOS. '
          'Use CmakeGenerator.ninja or CmakeGenerator.makefiles.',
        );

      case null:
        // Let CMake auto-select.
        break;
    }

    // User-supplied extra defines.
    args.addAll(cfg.extraDefines);

    return args;
  }

  /// Enforces a minimum iOS deployment target version.
  ///
  /// Parses [version] (e.g. `'13.0'`) and returns it unchanged if it is
  /// already >= [minMajor]. Otherwise returns `'$minMajor.0'`.
  static String _enforceMinimumIosVersion(String version, int minMajor) {
    final parts = version.split('.');
    final major = int.tryParse(parts.first) ?? 0;
    if (major >= minMajor) return version;
    return '$minMajor.0';
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
  // Android resolution (CMake auto-detection + NDK args)
  // -------------------------------------------------------------------------

  /// Resolves the CMake executable for Android builds.
  ///
  /// On Windows, when cmake is not explicitly configured and not on PATH,
  /// probes Visual Studio installation directories for a bundled CMake with
  /// version >= 3.25 (required by the vendored stdexec runtime).
  String _resolveAndroidCmake(AndroidConfig cfg) {
    if (cfg.cmake != 'cmake') {
      return cfg.cmake; // Explicitly configured by the caller.
    }
    if (!Platform.isWindows) {
      return cfg.cmake; // Non-Windows: rely on PATH.
    }
    final pathCmake = _whichOnPath('cmake.exe');
    if (pathCmake != null && _cmakeVersionOk(pathCmake)) {
      return pathCmake;
    }

    // Probe Visual Studio installations for a bundled CMake.
    for (final candidate in _windowsCmakeCandidates()) {
      if (!File(candidate).existsSync()) continue;
      if (_cmakeVersionOk(candidate)) {
        _log('resolved CMake for Android: $candidate');
        return candidate;
      }
    }

    // Nothing found — return 'cmake' and let _runProcess report the error.
    return cfg.cmake;
  }

  /// Checks that [cmakePath] reports a version >= 3.25.
  bool _cmakeVersionOk(String cmakePath) {
    try {
      final result = Process.runSync(cmakePath, ['--version']);
      if (result.exitCode != 0) return false;
      final output = result.stdout as String;
      // Typical output: "cmake version 3.28.3"
      final match = RegExp(r'(\d+)\.(\d+)').firstMatch(output);
      if (match == null) return false;
      final major = int.parse(match.group(1)!);
      final minor = int.parse(match.group(2)!);
      final ok = major > 3 || (major == 3 && minor >= 25);
      if (!ok) {
        _log('skipping $cmakePath (version ${match.group(0)} < 3.25)');
      }
      return ok;
    } on ProcessException {
      return false;
    }
  }

  /// Returns the directory containing a VS-bundled `ninja.exe`, or `null` if
  /// `ninja.exe` is already on PATH or no bundled copy is found.
  ///
  /// When CMake is resolved from a Visual Studio installation, the bundled
  /// `ninja.exe` lives in a sibling directory:
  /// ```text
  /// ...\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
  /// ...\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
  /// ```
  /// CMake's `-G Ninja` requires ninja on PATH.
  String? _findNinjaDir(String cmakePath) {
    if (_isOnPath('ninja.exe')) return null;

    // Derive the Ninja directory from the VS CMake layout.
    final binDir = File(cmakePath).parent; // ...\CMake\bin
    final cmakePkgDir = binDir.parent; // ...\CMake
    final extensionsDir = cmakePkgDir.parent; // ...\Microsoft\CMake
    final ninjaDir = '${extensionsDir.path}\\Ninja';
    if (File('$ninjaDir\\ninja.exe').existsSync()) {
      return ninjaDir;
    }
    return null;
  }

  /// Returns an environment map with the Ninja directory added to PATH,
  /// or `null` if no adjustment is needed.
  Map<String, String>? _ensureNinjaOnPath(String cmakePath) {
    final ninjaDir = _findNinjaDir(cmakePath);
    if (ninjaDir == null) return null;

    final env = Map<String, String>.from(Platform.environment);
    env['PATH'] = '$ninjaDir;${env['PATH'] ?? ''}';
    _log('added Ninja to PATH: $ninjaDir');
    return env;
  }

  /// Resolves the Android ABI from an explicit override or the hooks-provided
  /// target architecture.
  static String _resolveAndroidAbi(Architecture? arch, String? override) {
    final derived = switch (arch) {
      Architecture.arm64 => 'arm64-v8a',
      Architecture.arm => 'armeabi-v7a',
      Architecture.x64 => 'x86_64',
      Architecture.ia32 => 'x86',
      _ => null,
    };
    if (override != null) {
      const supported = {'arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86'};
      if (!supported.contains(override)) {
        throw DcbCMakeException(
          'Unsupported Android ABI override "$override". Supported ABIs: '
          '${supported.join(', ')}.',
        );
      }
      if (derived != null && derived != override) {
        throw DcbCMakeException(
          'Android ABI override "$override" does not match target '
          'architecture ${arch!.name} (expected "$derived").',
        );
      }
      if (arch != null && derived == null) {
        throw DcbCMakeException(
          'Android target architecture ${arch.name} is not supported by '
          'this builder.',
        );
      }
      return override;
    }
    if (derived == null) {
      final target = arch?.name ?? 'unknown';
      throw DcbCMakeException(
        'Android target architecture $target is unsupported. Supported '
        'architectures: arm64, arm, x64, ia32.',
      );
    }
    return derived;
  }

  /// Resolve an Android ABI using the same target/override rules as [run].
  ///
  /// Exposed for downstream hooks that need to validate a target before
  /// invoking their own build step.
  static String resolveAndroidAbi(Architecture? arch, [String? override]) =>
      _resolveAndroidAbi(arch, override);

  /// Resolves Android-specific configure arguments using the NDK toolchain.
  List<String> _resolveAndroidArgs(
    AndroidConfig cfg,
    String buildType,
    String abi,
  ) {
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
    args.add('-DANDROID_ABI=$abi');
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
    String abi,
  ) {
    // Map ABI to the NDK's target triple directory name.
    final abiDir = switch (abi) {
      'arm64-v8a' => 'aarch64-linux-android',
      'armeabi-v7a' => 'arm-linux-androideabi',
      'x86_64' => 'x86_64-linux-android',
      'x86' => 'i686-linux-android',
      _ => throw DcbCMakeException(
        'Unknown Android ABI: $abi. '
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
  _WindowsResolved _resolveWindows(
    WindowsConfig cfg,
    String buildType,
    Architecture? targetArchitecture,
  ) {
    final architecture = _resolveWindowsArchitecture(
      targetArchitecture,
      cfg._architectureOverride,
    );
    // Resolve VS installation root.
    final vsRoot = cfg.vsInstallPath ?? _detectVsInstallPath();

    // Resolve CMake executable.
    final cmakePath = _resolveWindowsCmake(cfg.cmake, vsRoot);

    final useClangCl = cfg.compiler == WindowsCompiler.clangCl;
    final useMsys2 =
        cfg.compiler == WindowsCompiler.msys2Clang ||
        cfg.compiler == WindowsCompiler.msys2Gcc;

    final args = <String>[];
    Map<String, String>? env;

    // Non-MSVC compilers need an explicit generator: default to Ninja (the
    // Visual Studio generator defaults to the MSVC toolset unless
    // `-T clangcl` is passed, so a bare auto-select would silently ignore
    // the compiler).
    final generator = cfg.generator ??
        (useClangCl || useMsys2 ? CmakeGenerator.ninja : null);
    if (cfg.generator == null && (useClangCl || useMsys2)) {
      _log('${cfg.compiler.name} selected; defaulting to Ninja generator');
    }

    // Generator.
    switch (generator) {
      case CmakeGenerator.msbuild:
        if (useMsys2) {
          throw DcbCMakeException(
            'CmakeGenerator.msbuild is not valid with ${cfg.compiler.name} '
            '(GNU-style toolchains require the Ninja generator). Use '
            'CmakeGenerator.ninja.',
          );
        }
        final vsVersion = _vsGeneratorName(vsRoot);
        args.addAll(['-G', vsVersion]);
        args.addAll(['-A', architecture]);
        if (useClangCl) {
          // Visual Studio generator + clang-cl toolset (requires the
          // "C++ Clang tools for Windows" VS component or a standalone LLVM
          // install; CMake locates clang-cl itself for this toolset).
          args.addAll(['-T', 'clangcl']);
        }

      case CmakeGenerator.ninja:
        args.addAll(['-G', 'Ninja']);
        args.add('-DCMAKE_BUILD_TYPE=$buildType');
        if (useMsys2) {
          // GNU-style MSYS2 toolchains (clang / gcc): self-contained
          // (headers + libraries shipped with MSYS2), no vcvars environment
          // needed. Only x64 is supported: ucrt64 is an x86_64 environment.
          if (architecture != 'x64') {
            throw DcbCMakeException(
              '${cfg.compiler.name} only supports architecture x64 '
              '(MSYS2 ucrt64 is an x86_64 environment); got '
              '"$architecture".',
            );
          }
          final isGcc = cfg.compiler == WindowsCompiler.msys2Gcc;
          final msys2Root = _resolveMsys2Root(cfg.msys2Path, needGcc: isGcc);
          final ucrt64Bin = '$msys2Root\\ucrt64\\bin';
          final cCompiler = isGcc ? '$ucrt64Bin\\gcc.exe' : '$ucrt64Bin\\clang.exe';
          final cxxCompiler =
              isGcc ? '$ucrt64Bin\\g++.exe' : '$ucrt64Bin\\clang++.exe';
          if (!File(cCompiler).existsSync() || !File(cxxCompiler).existsSync()) {
            throw DcbCMakeException(
              'MSYS2 ucrt64 ${isGcc ? 'gcc' : 'clang'} not found at: '
              '$ucrt64Bin (expected ${isGcc ? 'gcc/g++' : 'clang/clang++'}.exe).',
            );
          }
          args
            ..add('-DCMAKE_C_COMPILER=$cCompiler')
            ..add('-DCMAKE_CXX_COMPILER=$cxxCompiler');
          // MSYS2's own ninja / runtime DLLs live in ucrt64\bin.
          env = Map<String, String>.from(Platform.environment);
          env['PATH'] = '$ucrt64Bin;${env['PATH'] ?? ''}';
          _log('using MSYS2 ${isGcc ? 'gcc' : 'clang'}: $cxxCompiler');
          if (cfg.staticRuntime) {
            // Statically link libgcc / libstdc++ / winpthread so the output
            // DLL only depends on the system UCRT and needs no MSYS2 runtime
            // DLLs on PATH at runtime.
            const staticFlags = [
              '-static-libgcc',
              '-static-libstdc++',
              '-Wl,-Bstatic,--whole-archive',
              '-lwinpthread',
              '-Wl,--no-whole-archive,-Bdynamic',
            ];
            // Skip a variable when the caller already overrides it via
            // extraDefines (user values win over these defaults).
            void addIfNotOverridden(String define) {
              final name = define.substring(0, define.indexOf('='));
              if (cfg.extraDefines.any((d) => d.startsWith('$name='))) {
                _log('skipping default $name (overridden via extraDefines)');
                return;
              }
              args.add(define);
            }

            addIfNotOverridden('-DCMAKE_C_FLAGS=-static-libgcc');
            addIfNotOverridden(
              '-DCMAKE_CXX_FLAGS=-static-libgcc -static-libstdc++',
            );
            addIfNotOverridden(
              '-DCMAKE_EXE_LINKER_FLAGS=${staticFlags.join(' ')}',
            );
            addIfNotOverridden(
              '-DCMAKE_SHARED_LINKER_FLAGS=${staticFlags.join(' ')}',
            );
            _log('MSYS2 runtime statically linked (self-contained DLL)');
          }
        } else {
          // MSVC-based toolchains (cl.exe / clang-cl) need the MSVC
          // environment (cl.exe / link.exe on PATH, INCLUDE/LIB set).
          final vcEnv = _initVcEnvironment(cfg, vsRoot, architecture);
          env = Map<String, String>.from(vcEnv);
          final ninjaDir = _findNinjaDir(cmakePath);
          if (ninjaDir != null) {
            env['PATH'] = '$ninjaDir;${env['PATH'] ?? ''}';
            _log('added Ninja to PATH: $ninjaDir');
          }
          if (useClangCl) {
            // Detect clang-cl inside the vcvars environment first (a
            // VS-bundled clang-cl only becomes visible on PATH after
            // vcvarsall.bat), then fall back to the host PATH / LLVM installs.
            final clangCl = _resolveClangClPath(cfg.clangClPath, vcEnv: env);
            args
              ..add('-DCMAKE_C_COMPILER=$clangCl')
              ..add('-DCMAKE_CXX_COMPILER=$clangCl');
            // Make clang-cl resolvable by CMake even when it is not on PATH.
            final clangDir = File(clangCl).parent.path;
            if (env['PATH'] == null ||
                !env['PATH']!.split(';').contains(clangDir)) {
              env['PATH'] = '$clangDir;${env['PATH'] ?? ''}';
            }
            _log('using clang-cl: $clangCl');
          }
        }

      case CmakeGenerator.makefiles:
        throw DcbCMakeException(
          'CmakeGenerator.makefiles is not valid on Windows. '
          'Use CmakeGenerator.msbuild or CmakeGenerator.ninja.',
        );

      case null:
        // Let CMake auto-select (typically Visual Studio multi-config).
         // Still pass architecture for the VS generator.
         args.addAll(['-A', architecture]);
    }

    // CRT linkage: MSVC-style toolchains only. clang-cl accepts
    // CMAKE_MSVC_RUNTIME_LIBRARY; GNU-style MSYS2 clang links its own
    // runtime (libgcc / libstdc++ / winpthread) and would ignore it.
    if (!useMsys2) {
      final crtValue = _msvcRuntimeLibrary(cfg.dynamicCrt, buildType);
      args.add('-DCMAKE_MSVC_RUNTIME_LIBRARY=$crtValue');
    }

    // User-supplied extra defines.
    args.addAll(cfg.extraDefines);

    return _WindowsResolved(
      cmakePath: cmakePath,
      configureArgs: args,
      architecture: architecture,
      environment: env,
    );
  }

  static String _resolveWindowsArchitecture(
    Architecture? targetArchitecture,
    String? override,
  ) {
    final derived = switch (targetArchitecture) {
      Architecture.x64 => 'x64',
      Architecture.arm64 => 'arm64',
      _ => null,
    };
    if (override != null) {
      if (override != 'x64' && override != 'arm64') {
        throw DcbCMakeException(
          'Unsupported Windows architecture override "$override". '
          'Supported architectures: x64, arm64.',
        );
      }
      if (derived != null && derived != override) {
        throw DcbCMakeException(
          'Windows architecture override "$override" does not match target '
          'architecture ${targetArchitecture!.name} (expected "$derived").',
        );
      }
      if (targetArchitecture != null && derived == null) {
        throw DcbCMakeException(
          'Windows target architecture ${targetArchitecture.name} is not '
          'supported by this builder.',
        );
      }
      return override;
    }
    if (derived == null && targetArchitecture != null) {
      throw DcbCMakeException(
        'Windows target architecture ${targetArchitecture.name} is '
        'unsupported. Supported architectures: x64, arm64.',
      );
    }
    return derived ?? 'x64';
  }

  /// Resolve a Windows CMake architecture using the same target/override
  /// rules as [run].
  static String resolveWindowsArchitecture(
    Architecture? targetArchitecture, [String? override]
  ) => _resolveWindowsArchitecture(targetArchitecture, override);

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
  ///
  /// Runs the batch file from its own directory so that we never have to pass
  /// a quoted path to `cmd.exe /C`. `cmd.exe` strips the first and last quote
  /// from a `/C` command line when the text between those quotes contains
  /// shell metacharacters, which breaks paths that contain spaces (e.g.
  /// `C:\Program Files\Microsoft Visual Studio\...`). Keeping the unquoted
  /// filename `vcvarsall.bat` and changing the working directory avoids that.
  ///
  /// If the caller already ran `vcvarsall.bat` (detected via `VSCMD_VER` or
  /// via `LIB`/`PATH` containing Windows SDK / MSVC paths), the existing
  /// environment is used as-is. This is useful in CI pipelines that set the
  /// environment once and do not want the hook to re-select a VS installation.
  Map<String, String> _initVcEnvironment(
    WindowsConfig cfg,
    String? vsRoot,
    String architecture,
  ) {
    if (_hasVcEnvironment()) {
      _log('MSVC environment already present; skipping vcvarsall.bat');
      return Platform.environment;
    }

    final vcvarsall = _findVcvarsall(vsRoot);
    if (vcvarsall == null) {
      _log('WARNING: vcvarsall.bat not found; Ninja may fail to locate cl.exe');
      return Platform.environment;
    }

    final arch = architecture == 'arm64' ? 'x64_arm64' : 'x64';
    _log('initializing MSVC env: $vcvarsall $arch');

    // Run vcvarsall.bat from its own directory to avoid the cmd.exe /C quote-
    // stripping behavior that breaks paths containing spaces.
    final vcvarsDir = File(vcvarsall).parent.path;
    final result = Process.runSync(
      'cmd.exe',
      ['/C', 'vcvarsall.bat $arch >nul 2>&1 && set'],
      workingDirectory: vcvarsDir,
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
    if (!env.containsKey('LIB')) {
      _log('WARNING: vcvarsall.bat did not set LIB; linker may fail');
    }
    return env;
  }

  /// Whether the current process already has a VS developer environment.
  ///
  /// Detects:
  /// - `VSCMD_VER` set by `vcvarsall.bat` / `vcvars64.bat`.
  /// - `LIB` containing the Windows SDK / MSVC library path.
  /// - `PATH` containing the MSVC `VC\Tools\MSVC` compiler directory.
  bool _hasVcEnvironment() {
    final env = Platform.environment;
    if (env['VSCMD_VER']?.isNotEmpty ?? false) return true;

    final lib = (env['LIB'] ?? '').toLowerCase();
    final path = (env['PATH'] ?? '').toLowerCase();
    return lib.contains(r'windows kits') && path.contains(r'vc\tools\msvc');
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
  List<File> _bundleWindowsCrt(
    WindowsConfig cfg,
    File outputDll,
    String architecture,
  ) {
    final vsRoot = cfg.vsInstallPath ?? _detectVsInstallPath();
    if (vsRoot == null) {
      _log('WARNING: cannot bundle CRT — VS installation not found');
      return const [];
    }

    final redistBase = Directory('$vsRoot\\VC\\Redist\\MSVC');
    if (!redistBase.existsSync()) {
      _log('WARNING: cannot bundle CRT — Redist directory not found');
      return const [];
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
      return const [];
    }

    final arch = architecture == 'arm64' ? 'arm64' : 'x64';
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
      return const [];
    }

    const dllNames = ['MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll'];

    final outDir = outputDll.parent;
    final bundled = <File>[];
    var copied = 0;
    for (final name in dllNames) {
      final src = File('${sourceDir.path}\\$name');
      if (src.existsSync()) {
        final destination = File('${outDir.path}\\$name');
        src.copySync(destination.path);
        bundled.add(destination);
        copied++;
      }
    }
    _log('bundled $copied CRT DLL(s) from ${sourceDir.path}');
    return bundled;
  }

  /// Copies the MSYS2 runtime DLLs next to [outputDll].
  ///
  /// Used for [WindowsCompiler.msys2Clang] / [WindowsCompiler.msys2Gcc]
  /// builds with [WindowsConfig.staticRuntime] = `false`: the DLL then
  /// depends on `libgcc_s_seh-1.dll`, `libstdc++-6.dll`, and
  /// `libwinpthread-1.dll` from `<msys2>\ucrt64\bin`. Only the DLLs that
  /// actually exist in the toolchain are copied.
  List<File> _bundleMsys2Runtime(WindowsConfig cfg, File outputDll) {
    final root = _resolveMsys2Root(
      cfg.msys2Path,
      needGcc: cfg.compiler == WindowsCompiler.msys2Gcc,
    );
    final sourceDir = '$root\\ucrt64\\bin';

    const dllNames = [
      'libgcc_s_seh-1.dll',
      'libstdc++-6.dll',
      'libwinpthread-1.dll',
    ];

    final outDir = outputDll.parent;
    final bundled = <File>[];
    var copied = 0;
    for (final name in dllNames) {
      final src = File('$sourceDir\\$name');
      if (src.existsSync()) {
        final destination = File('${outDir.path}\\$name');
        src.copySync(destination.path);
        bundled.add(destination);
        copied++;
      }
    }
    _log('bundled $copied MSYS2 runtime DLL(s) from $sourceDir');
    return bundled;
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
  ///
  /// MinGW-style toolchains (e.g. MSYS2 clang) prefix shared libraries with
  /// `lib`, so `lib<fileName>` is also probed.
  File _locateArtifact(Uri buildDir, String fileName, String buildType) {
    final candidates = <Uri>[
      buildDir.resolve('$buildType/').resolve(fileName),
      buildDir.resolve(fileName),
      // MinGW-style shared libraries: lib<name>.dll instead of <name>.dll.
      buildDir.resolve('$buildType/').resolve('lib$fileName'),
      buildDir.resolve('lib$fileName'),
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

  /// Copies `compile_commands.json` from the CMake build directory to
  /// [relativeDest] under the package root if it exists.
  ///
  /// Searches the single-config root and the multi-config subdirectories
  /// (`Release/`, `Debug/`). Copy failures are logged as warnings and do not
  /// fail the build.
  void _copyCompileCommandsIfPresent({
    required Uri buildDir,
    required String buildType,
    required Uri packageRoot,
    required String relativeDest,
  }) {
    final candidates = <Uri>[
      buildDir.resolve('compile_commands.json'),
      buildDir.resolve('$buildType/compile_commands.json'),
    ];
    for (final candidate in candidates) {
      final source = File.fromUri(candidate);
      if (!source.existsSync()) continue;

      final dest = File.fromUri(packageRoot.resolve(relativeDest));
      try {
        dest.parent.createSync(recursive: true);
        source.copySync(dest.path);
        _log('copied compile_commands.json to ${dest.path}');
      } on FileSystemException catch (e) {
        _log('WARNING: failed to copy compile_commands.json: $e');
      }
      return;
    }
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
    // Use an explicit directory stack so generated/build trees are pruned
    // before traversal. Directory.listSync(recursive: true) enumerates every
    // descendant first, which makes a package-root CMake project repeatedly
    // walk .dart_tool/hooks_runner, CMake _deps, and prior build outputs.
    const skippedDirectories = {
      '.dart_tool',
      '.git',
      '.idea',
      '.vscode',
      '_deps',
      'build',
      'dist',
      'out',
      'cmake-build-debug',
      'cmake-build-release',
      'dcb_build',
      'node_modules',
    };
    final pending = <Directory>[rootDir];
    while (pending.isNotEmpty) {
      final directory = pending.removeLast();
      for (final entity in directory.listSync(followLinks: false)) {
        if (entity is Directory) {
          final name = entity.uri.pathSegments.last.toLowerCase();
          if (skippedDirectories.contains(name) ||
              name.startsWith('build_')) {
            continue;
          }
          pending.add(entity);
          continue;
        }
        if (entity is! File) {
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
    final pathCmake = _whichOnPath('cmake.exe');
    if (pathCmake != null && _cmakeVersionOk(pathCmake)) {
      return pathCmake;
    }

    // Try the resolved VS root first.
    if (vsRoot != null) {
      const cmakeRelative =
          r'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe';
      final candidate = '$vsRoot\\$cmakeRelative';
      if (File(candidate).existsSync() && _cmakeVersionOk(candidate)) {
        _log('resolved CMake: $candidate');
        return candidate;
      }
    }

    // Fallback: standalone installs and vswhere discovery.
    for (final candidate in _windowsCmakeCandidates()) {
      if (File(candidate).existsSync() && _cmakeVersionOk(candidate)) {
        _log('resolved CMake: $candidate');
        return candidate;
      }
    }
    return configured;
  }

  /// Whether [exeName] resolves against a directory listed in `PATH`.
  bool _isOnPath(String exeName) => _whichOnPath(exeName) != null;

  /// Returns the full path to [exeName] found on `PATH`, or `null` when
  /// [exeName] does not resolve against any `PATH` directory.
  ///
  /// When [environment] is provided, its `PATH` is searched instead of the
  /// current process environment — this is how clang-cl is looked up inside
  /// the MSVC environment initialized by `vcvarsall.bat` (which puts the
  /// VS-bundled clang-cl on PATH when the "C++ Clang tools for Windows"
  /// component is installed).
  String? _whichOnPath(String exeName, {Map<String, String>? environment}) {
    final pathVar = (environment ?? Platform.environment)['PATH'] ?? '';
    for (final dir in pathVar.split(';')) {
      if (dir.isEmpty) {
        continue;
      }
      final candidate = '$dir\\$exeName';
      if (File(candidate).existsSync()) {
        return candidate;
      }
    }
    return null;
  }

  /// Resolves the `clang-cl.exe` executable for [WindowsCompiler.clangCl]
  /// builds.
  ///
  /// Priority:
  /// 1. explicit [configured] path (must exist — no fallback);
  /// 2. the MSVC environment at [vcEnv] (the `vcvarsall.bat` output, which
  ///    puts the VS-bundled clang-cl on `PATH` when the "C++ Clang tools for
  ///    Windows" component is installed);
  /// 3. the current process `PATH`;
  /// 4. well-known LLVM install locations (`C:\Program Files\LLVM\bin\
  ///    clang-cl.exe` and the x86 variant);
  /// 5. the LLVM registry key (`HKLM\SOFTWARE\WOW6432Node\LLVM\LLVM` and
  ///    `HKLM\SOFTWARE\LLVM\LLVM`, written by the LLVM installer).
  ///
  /// Throws [DcbCMakeException] with guidance when clang-cl cannot be found.
  String _resolveClangClPath(String? configured, {Map<String, String>? vcEnv}) {
    if (configured != null) {
      final explicit = File(configured);
      if (explicit.existsSync()) {
        return explicit.path;
      }
      throw DcbCMakeException(
        'WindowsConfig.clangClPath does not exist: $configured',
      );
    }

    // VS-bundled clang-cl (only visible inside the vcvars environment).
    final vsClangCl = _whichOnPath('clang-cl.exe', environment: vcEnv);
    if (vsClangCl != null) {
      _log('using VS-bundled clang-cl: $vsClangCl');
      return vsClangCl;
    }

    final onPath = _whichOnPath('clang-cl.exe');
    if (onPath != null) {
      return onPath;
    }

    for (final candidate in const [
      r'C:\Program Files\LLVM\bin\clang-cl.exe',
      r'C:\Program Files (x86)\LLVM\bin\clang-cl.exe',
    ]) {
      if (File(candidate).existsSync()) {
        return candidate;
      }
    }

    // Fall back to the registry key written by the LLVM installer
    // (checked in both the 32-bit and 64-bit registry views).
    for (final key in const [
      r'HKLM\SOFTWARE\WOW6432Node\LLVM\LLVM',
      r'HKLM\SOFTWARE\LLVM\LLVM',
    ]) {
      try {
        final result = Process.runSync('reg.exe', [
          'query',
          key,
          '/v',
          'LLVM',
        ]);
        if (result.exitCode == 0) {
          // reg.exe writes UTF-16LE to pipes; Dart decodes it with the
          // system code page, so ASCII chars arrive as "X\u0000". Strip the
          // NUL bytes to recover readable text.
          final raw = result.stdout as String;
          final output = raw.contains('\u0000')
              ? raw.replaceAll('\u0000', '')
              : raw;
          final match = RegExp(
            r'LLVM\s+REG_SZ\s+(.+?)\s*$',
            multiLine: true,
          ).firstMatch(output);
          if (match != null) {
            final fromRegistry = '${match.group(1)!.trim()}\\bin\\clang-cl.exe';
            if (File(fromRegistry).existsSync()) {
              return fromRegistry;
            }
          }
        }
      } on ProcessException {
        // Ignore — error out below with guidance.
      }
    }

    throw DcbCMakeException(
      'Could not locate clang-cl.exe for WindowsCompiler.clangCl. '
      'Install LLVM (https://llvm.org) or pass an explicit path via '
      'WindowsConfig(clangClPath: r"C:\\path\\to\\clang-cl.exe").',
    );
  }

  /// Resolves the MSYS2 installation root for [WindowsCompiler.msys2Clang] /
  /// [WindowsCompiler.msys2Gcc] builds.
  ///
  /// Priority: explicit [configured] path → `MSYS2_ROOT` environment
  /// variable → well-known install locations (`C:\msys64`, `C:\msys2`,
  /// `D:\msys2`). The candidate is accepted when the ucrt64 toolchain is
  /// present at `<root>\ucrt64\bin\clang++.exe` (or `g++.exe` when
  /// [needGcc] is `true`).
  ///
  /// An explicit [configured] path that does not contain the toolchain is an
  /// error (no silent fallback, mirroring [WindowsConfig.clangClPath]);
  /// otherwise throws [DcbCMakeException] with guidance when no MSYS2
  /// toolchain is found.
  String _resolveMsys2Root(String? configured, {required bool needGcc}) {
    final cxxExe = needGcc ? 'g++.exe' : 'clang++.exe';
    if (configured != null) {
      if (File('$configured\\ucrt64\\bin\\$cxxExe').existsSync()) {
        return configured;
      }
      throw DcbCMakeException(
        'WindowsConfig.msys2Path does not contain an MSYS2 ucrt64 '
        '${needGcc ? 'gcc' : 'clang'} toolchain: $configured '
        '(expected <root>\\ucrt64\\bin\\$cxxExe)',
      );
    }

    final envRoot = Platform.environment['MSYS2_ROOT'];
    final candidates = <String>[
      if (envRoot != null && envRoot.isNotEmpty) envRoot,
      r'C:\msys64',
      r'C:\msys2',
      r'D:\msys2',
    ];
    for (final root in candidates) {
      if (File('$root\\ucrt64\\bin\\$cxxExe').existsSync()) {
        return root;
      }
    }
    throw DcbCMakeException(
      'Could not locate an MSYS2 ucrt64 ${needGcc ? 'gcc' : 'clang'} '
      'toolchain for ${needGcc ? 'WindowsCompiler.msys2Gcc' : 'WindowsCompiler.msys2Clang'}. '
      'Install MSYS2 (https://www.msys2.org) with the '
      '${needGcc ? 'mingw-w64-ucrt-x86_64-gcc' : 'mingw-w64-ucrt-x86_64-clang'} '
      'package, or pass an explicit root via '
      'WindowsConfig(msys2Path: r"D:\\msys2").',
    );
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
