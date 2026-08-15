---
title: Native Assets Build Hooks
description: How dart_cpp_bridge compiles and bundles C++ libraries through
  Native Assets build hooks
slug: v1/guides/fundamentals/native-assets-hooks
---

`dart_cpp_bridge` uses Dart's **Native Assets** mechanism to build and bundle the C++ library automatically. Instead of manually compiling a DLL / `.so` / `.dylib` and distributing it, you write a small `hook/build.dart` that invokes `DcbCMakeBuilder`. The Dart / Flutter tooling then runs this hook during `dart run`, `flutter run`, or `flutter build`, producing a bundled \[CodeAsset] that the generated FFI bindings load at runtime.

## What the hook does

`hook/build.dart` is a normal Dart program executed by the Native Assets pipeline. It receives a `BuildInput` describing the target platform and produces a `BuildOutput` containing declared code assets. For dart\_cpp\_bridge, the hook:

1. Selects a platform config (`WindowsConfig`, `LinuxConfig`, `MacosConfig`, `IosConfig`, or `AndroidConfig`) based on `input.config.code.targetOS`.
2. Invokes `DcbCMakeBuilder` with that config, the CMake `sourceDir`, the desired asset name, and the library base name.
3. Lets the builder configure, compile, locate, and emit the native library as a `CodeAsset`.

Dart / Flutter takes care of caching, invalidation, and bundling the resulting library into the app or package.

## Minimal hook example

```dart title="hook/build.dart"
import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    final config = switch (input.config.code.targetOS) {
      OS.windows => WindowsConfig(),
      OS.linux => LinuxConfig(),
      OS.macOS => MacosConfig(),
      OS.iOS => IosConfig(),
      OS.android => AndroidConfig(
          ndkPath: r'C:\Users\<you>\AppData\Local\Android\Sdk\ndk\<version>',
        ),
      final os => throw UnsupportedError('Unsupported target platform: $os'),
    };

    await DcbCMakeBuilder(
      config: config,
      sourceDir: 'native',
      assetName: 'src/native_gen/dcb_bindings.dart',
      libName: 'my_project',
    ).run(input: input, output: output);
  });
}
```

Save this as `hook/build.dart` at the project root. The `sourceDir` points at the directory containing `CMakeLists.txt` (commonly `native/`). `assetName` is the identifier used by `@Native(assetId: 'package:<package>/<assetName>')` in the generated bindings. `libName` is the CMake output name (e.g. `my_project.dll`, `libmy_project.so`).

## DcbCMakeBuilder

`DcbCMakeBuilder` is the reusable CMake-driven builder exposed by `package:dart_cpp_bridge/hook.dart`. It performs three steps inside `input.outputDirectory/dcb_build/`:

1. **Configure** — `cmake -S <sourceDir> -B <buildDir> [generator] [defines]`.
2. **Build** — `cmake --build <buildDir> --config <Release|Debug> [--parallel]`.
3. **Emit** — locate the produced library and add it to `output.assets.code` as a `CodeAsset`.

Constructor options:

| Parameter | Type | Default | Purpose |
|---|---|---|---|
| `config` | `DcbPlatformConfig` | required | Platform-specific config (`WindowsConfig`, `LinuxConfig`, `MacosConfig`, `IosConfig`, `AndroidConfig`). |
| `assetName` | `String` | required | Code asset identifier consumed by generated FFI bindings. |
| `assetPackage` | `String?` | building package | Override the package namespace of the emitted asset. Use `'dart_cpp_bridge'` when embedding the runtime via `WHOLE_ARCHIVE`. |
| `sourceDir` | `String?` | package root | CMake source directory relative to the package root. Usually `'native'`. |
| `libName` | `String?` | package name | CMake library base name. Must match `add_library(<name> ...)` in `CMakeLists.txt`. |
| `extraDefines` | `List<String>` | `[]` | Extra `-D` flags passed to the CMake configure step. Applied on all platforms. |
| `buildOptions` | `DcbBuildOptions` | `DcbBuildOptions()` | Debug/release override, parallel build toggle, and `compile_commands.json` control. |

## CMake generator selection

`CmakeGenerator` selects the CMake build-system generator. The right choice depends on the platform and whether the host has the matching toolchain on PATH.

| Generator | Value | Platforms | Notes |
|---|---|---|---|
| `msbuild` | `CmakeGenerator.msbuild` | Windows only | Visual Studio multi-config generator. Requires no extra PATH setup; CMake auto-detects MSBuild. |
| `ninja` | `CmakeGenerator.ninja` | All | Fast single-config generator. On Windows requires the MSVC environment; the builder invokes `vcvarsall.bat` automatically. |
| `makefiles` | `CmakeGenerator.makefiles` | Linux, macOS, iOS | Unix Makefiles single-config generator. Always available but slower than Ninja. |

* On **Windows**, leaving `generator` as `null` lets CMake auto-select the Visual Studio generator. Use `ninja` for faster incremental builds.
* On **Linux / macOS / iOS**, leaving `generator` as `null` lets CMake pick Unix Makefiles. Set `ninja` when `ninja` is on PATH.
* On **Android**, `ninja` is the default because the NDK bundles it.

When a generator path is required but not on PATH, pass `generatorPath` (e.g. `ninja.exe` or `MSBuild.exe`).

## Platform configs

Each platform has a dedicated config class controlling generator, compiler, runtime linkage, and cross-compilation settings.

### Windows (`WindowsConfig`)

```dart
WindowsConfig(
  cmake: 'cmake',
  dynamicCrt: true,      // true = /MD, false = /MT (MSVC/clang-cl only)
  bundleCrt: true,      // copy MSVCP140 / VCRUNTIME140 next to the DLL
  vsInstallPath: r'C:\Program Files\Microsoft Visual Studio\2022\Community',
  architecture: 'x64',    // 'x64' or 'arm64'
  generator: CmakeGenerator.ninja,
  generatorPath: null,
  compiler: WindowsCompiler.msvc, // msvc | clangCl | msys2Clang | msys2Gcc
  clangClPath: null,      // explicit clang-cl.exe path when compiler is clangCl
  msys2Path: null,        // explicit MSYS2 root when compiler is msys2Clang/Gcc
  staticRuntime: true,    // statically link MSYS2 C++ runtime (self-contained DLL)
  extraDefines: const [],
)
```

Field reference:

| Field | Type | Default | Description |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake executable. Absolute path or command resolved via PATH. |
| `dynamicCrt` | `bool` | `true` | Link the MSVC C runtime dynamically (`/MD`) when `true`, or statically (`/MT`) when `false`. MSVC-style toolchains only (`msvc` / `clangCl`). |
| `bundleCrt` | `bool` | `true` | Copy the matching runtime DLLs next to the output DLL: MSVC CRT DLLs (`MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`) for `msvc`/`clangCl` when `dynamicCrt` is `true`; MSYS2 runtime DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll`) for `msys2Clang`/`msys2Gcc` when `staticRuntime` is `false`. |
| `vsInstallPath` | `String?` | `null` | Visual Studio / Build Tools installation root. When `null`, the builder auto-detects via `vswhere.exe`, then falls back to well-known paths. |
| `architecture` | `String` | `'x64'` | Target architecture passed to CMake `-A`. Supported: `'x64'`, `'arm64'`. |
| `generator` | `CmakeGenerator?` | `null` | CMake generator. `null` lets CMake auto-select (typically Visual Studio / MSBuild on Windows; Ninja when `compiler` is `clangCl` / `msys2Clang` / `msys2Gcc`). |
| `generatorPath` | `String?` | `null` | Explicit path to the generator executable. For `ninja`: path to `ninja.exe`. For `msbuild`: path to `MSBuild.exe`. |
| `compiler` | `WindowsCompiler` | `msvc` | `msvc` = MSVC `cl.exe` (default). `clangCl` = LLVM `clang-cl` (MSVC-compatible). `msys2Clang` / `msys2Gcc` = GNU/MinGW-style MSYS2 ucrt64 clang / gcc. |
| `clangClPath` | `String?` | `null` | Explicit path to `clang-cl.exe` when `compiler` is `clangCl`. When `null`, the builder auto-detects after vcvars: VS-bundled clang-cl first, then PATH → `C:\Program Files\LLVM\bin\clang-cl.exe` → LLVM registry key. |
| `msys2Path` | `String?` | `null` | Explicit MSYS2 root (e.g. `D:\msys2`) when `compiler` is `msys2Clang` / `msys2Gcc`. When `null`, resolved from `MSYS2_ROOT` → `C:\msys64` → `C:\msys2` → `D:\msys2`. |
| `staticRuntime` | `bool` | `true` | MSYS2 toolchains only: statically link libgcc / libstdc++ / winpthread into the DLL (self-contained, no runtime DLLs needed on PATH). When `false`, the MSYS2 runtime DLLs are bundled next to the output if `bundleCrt` is `true`. |
| `extraDefines` | `List<String>` | `[]` | Extra `-D` definitions passed to the CMake configure step. |

Key behaviors:

* `dynamicCrt: true` produces a DLL that depends on the MSVC runtime. Set `bundleCrt: true` to copy the correct runtime DLLs next to the output so the app loads the matching version.
* `dynamicCrt: false` links the CRT statically (`/MT`) for a self-contained DLL.
* `vsInstallPath` is used both for selecting the CMake/MSBuild generator and for locating `vcvarsall.bat` when Ninja is requested.
* `CmakeGenerator.ninja` requires the MSVC environment. The builder calls `vcvarsall.bat <arch>` from the resolved VS installation automatically, but only when the current process does not already have a VS developer environment (`VSCMD_VER`, `LIB`, or `PATH` containing MSVC paths).
* `compiler: WindowsCompiler.clangCl` builds with LLVM `clang-cl`. With `CmakeGenerator.ninja` (the default for clang-cl when `generator` is `null`), the builder initializes the MSVC environment via `vcvarsall.bat` first, then locates clang-cl: an explicit `clangClPath` wins, then a VS-bundled clang-cl (visible on the vcvars PATH when the "C++ Clang tools for Windows" component is installed), then PATH / LLVM installs / the LLVM registry key. The resolved compiler is passed as `-DCMAKE_C(XX)_COMPILER=<clang-cl>` and its directory is added to `PATH` for the CMake process, so clang-cl does not need to be on PATH globally. With `CmakeGenerator.msbuild`, the builder passes `-T clangcl` to select the clang-cl toolset inside Visual Studio.
* `compiler: WindowsCompiler.msys2Clang` / `msys2Gcc` build with the GNU/MinGW-style MSYS2 ucrt64 clang / gcc (`x86_64-w64-windows-gnu` target, `mingw-w64-ucrt-x86_64-clang` / `-gcc` packages). Only the Ninja generator is supported (the default when `generator` is `null`). The MSYS2 root is resolved from `msys2Path` → `MSYS2_ROOT` → `C:\msys64` → `C:\msys2` → `D:\msys2`, and `-DCMAKE_C(XX)_COMPILER=<root>\ucrt64\bin\clang(++).exe` (or `gcc(++).exe`) is passed; no vcvars environment is needed. By default (`staticRuntime: true`) libgcc / libstdc++ / winpthread are statically linked into the DLL, so the output only depends on the system UCRT and needs no MSYS2 runtime DLLs on PATH at runtime. With `staticRuntime: false` the output depends on the MSYS2 runtime DLLs (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`, `libwinpthread-1.dll` from `<root>\ucrt64\bin`) — they are bundled next to the output when `bundleCrt` is `true`.

### Linux (`LinuxConfig`)

```dart
LinuxConfig(
  cmake: 'cmake',
  generator: CmakeGenerator.ninja,
  generatorPath: null,
  compiler: '/usr/bin/clang++',
  toolchainFile: null,
  staticLibStdCpp: true,
  extraDefines: const [],
)
```

Field reference:

| Field | Type | Default | Description |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake executable. |
| `generator` | `CmakeGenerator?` | `null` | `ninja` or `makefiles`. `null` lets CMake auto-select (usually Unix Makefiles). |
| `generatorPath` | `String?` | `null` | Explicit path to `ninja` or `make` when not on PATH. |
| `compiler` | `String?` | `null` | C++ compiler executable (e.g. `/usr/bin/clang++`). Passed as `-DCMAKE_CXX_COMPILER=<path>`. |
| `toolchainFile` | `String?` | `null` | CMake toolchain file for cross-compilation. Passed as `-DCMAKE_TOOLCHAIN_FILE=<path>`. Takes precedence over `compiler`. |
| `staticLibStdCpp` | `bool` | `true` | Statically link `libstdc++` into the output `.so` so it does not depend on the target system's `libstdc++.so.6` version. |
| `extraDefines` | `List<String>` | `[]` | Extra `-D` definitions passed to CMake. |

Key behaviors:

* Defaults to static `libstdc++` linkage so the `.so` is self-contained.
* `toolchainFile` can be used for cross-compilation; it takes precedence over `compiler`.

### macOS (`MacosConfig`)

```dart
MacosConfig(
  cmake: 'cmake',
  generator: CmakeGenerator.ninja,
  generatorPath: null,
  compiler: null,
  deploymentTarget: '13.0',
  universal: false,
  extraDefines: const [],
)
```

Field reference:

| Field | Type | Default | Description |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake executable. |
| `generator` | `CmakeGenerator?` | `null` | `ninja` or `makefiles`. `null` lets CMake auto-select. |
| `generatorPath` | `String?` | `null` | Explicit path to `ninja` or `make`. |
| `compiler` | `String?` | `null` | C++ compiler executable. `null` uses AppleClang. Passed as `-DCMAKE_CXX_COMPILER=<path>`. |
| `deploymentTarget` | `String?` | `null` | Minimum macOS deployment target (e.g. `'13.0'`). Maps to `CMAKE_OSX_DEPLOYMENT_TARGET`. |
| `universal` | `bool` | `false` | Build a universal `arm64 + x86_64` binary. Sets `CMAKE_OSX_ARCHITECTURES=arm64;x86_64`. |
| `extraDefines` | `List<String>` | `[]` | Extra `-D` definitions. |

Key behaviors:

* Single-config generator (Ninja or Unix Makefiles). `CMAKE_BUILD_TYPE` is set at configure time.
* `deploymentTarget` maps to `CMAKE_OSX_DEPLOYMENT_TARGET`.
* `universal: true` adds both `arm64` and `x86_64` architectures.

### iOS (`IosConfig`)

```dart
IosConfig(
  cmake: 'cmake',
  generator: CmakeGenerator.ninja,
  generatorPath: null,
  developerDir: null,
  deploymentTarget: null,
  extraDefines: const [],
)
```

Field reference:

| Field | Type | Default | Description |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake executable. |
| `generator` | `CmakeGenerator?` | `null` | `ninja` or `makefiles`. |
| `generatorPath` | `String?` | `null` | Explicit path to `ninja` or `make`. |
| `developerDir` | `String?` | `null` | Xcode developer directory. When set, passed via the `DEVELOPER_DIR` environment variable. `null` uses `xcode-select -p`. |
| `deploymentTarget` | `String?` | `null` | Minimum iOS deployment target override (e.g. `'15.0'`). `null` uses `input.config.code.iOS.targetVersion`. |
| `extraDefines` | `List<String>` | `[]` | Extra `-D` definitions. |

Key behaviors:

* Cross-compiles with `-DCMAKE_SYSTEM_NAME=iOS`.
* The hooks system tells the builder whether the target is device (`iphoneos`) or simulator (`iphonesimulator`), and the target architecture.
* iOS requires static linking; the builder honors the hooks `linkModePreference`.
* The minimum deployment target is floored at `14.0` because `async_simple` uses `std::atomic::wait/notify`.

### Android (`AndroidConfig`)

```dart
AndroidConfig(
  cmake: 'cmake',
  ndkPath: r'C:\Users\<you>\AppData\Local\Android\Sdk\ndk\29.0.14206865',
  abi: null,              // null = derive from targetArchitecture
  androidPlatform: 21,
  staticStl: true,
  generator: CmakeGenerator.ninja,
  extraDefines: const [],
)
```

Field reference:

| Field | Type | Default | Description |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake executable. On Windows, if not on PATH, the builder probes the VS-bundled CMake. |
| `ndkPath` | `String` | required | Android NDK root directory. The builder derives the toolchain file from `<ndkPath>/build/cmake/android.toolchain.cmake`. |
| `abi` | `String?` | `null` | Target ABI. `null` derives from `input.config.code.targetArchitecture`: `arm64` → `arm64-v8a`, `arm` → `armeabi-v7a`, `x64` → `x86_64`, `ia32` → `x86`. |
| `androidPlatform` | `int` | `21` | Minimum Android API level. Passed as `-DANDROID_PLATFORM=android-<level>`. |
| `staticStl` | `bool` | `true` | `true` links `c++_static`; `false` links `c++_shared` and registers `libc++_shared.so` as an additional code asset. |
| `generator` | `CmakeGenerator?` | `CmakeGenerator.ninja` | CMake generator. The NDK bundles Ninja, so Ninja is the default. |
| `extraDefines` | `List<String>` | `[]` | Extra `-D` definitions. |

Key behaviors:

* Uses the NDK's CMake toolchain file at `<ndkPath>/build/cmake/android.toolchain.cmake`.
* `staticStl: true` links `c++_static` (self-contained `.so`).
* `staticStl: false` links `c++_shared` and the builder registers `libc++_shared.so` as an additional code asset so it is packaged into the APK.
* On Windows, if `cmake` is not on PATH the builder probes the Visual Studio CMake bundled with the IDE. When using Ninja on Windows, the builder also ensures the bundled `ninja.exe` is on PATH.

## Build options

`DcbBuildOptions` controls behavior across platforms:

```dart
DcbBuildOptions(
  debug: null,                         // null = auto-detect from linkingEnabled
  parallel: true,                      // pass --parallel to cmake --build
  copyCompileCommands: true,           // generate and copy compile_commands.json
  compileCommandsPath: 'compile_commands.json', // relative to package root
)
```

Field reference:

| Field | Type | Default | Description |
|---|---|---|---|
| `debug` | `bool?` | `null` | Force Debug (`true`) or Release (`false`). `null` infers from `input.config.linkingEnabled`. |
| `parallel` | `bool` | `true` | Pass `--parallel` to `cmake --build`, enabling multi-file compilation. |
| `copyCompileCommands` | `bool` | `true` | Pass `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` and copy the generated `compile_commands.json` after a successful build. |
| `compileCommandsPath` | `String` | `'compile_commands.json'` | Relative path under the package root where `compile_commands.json` is copied. Parent directories are created automatically. |

When `debug` is `null`, the builder infers it from `input.config.linkingEnabled`:

* `linkingEnabled == true` → AOT / release → `Release`
* `linkingEnabled == false` → JIT / debug → `Debug`

This matches the Native Assets convention used by `dart run` versus `dart compile` / Flutter release builds.

### compile\_commands.json

By default, the builder:

1. Passes `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to CMake (for generators that support it, such as Ninja or Makefiles).
2. After a successful build, copies the generated `compile_commands.json` to the package root next to `pubspec.yaml`.

To place the file elsewhere, set `compileCommandsPath` to a relative path:

```dart
const DcbBuildOptions(
  copyCompileCommands: true,
  compileCommandsPath: 'build/compile_commands.json',
)
```

The destination is resolved relative to the package root, and any missing parent directories are created automatically. Set `copyCompileCommands: false` to disable generation and copying entirely.

## Asset name and package

The generated FFI bindings load the native library through a `@Native` annotation like:

```dart
@Native<IntPtr Function()>(assetId: 'package:my_project/src/native_gen/dcb_bindings.dart')
```

`DcbCMakeBuilder` emits a `CodeAsset` whose `name` is the `assetName` you passed, and whose `package` defaults to the building package. The builder also supports `assetPackage: 'dart_cpp_bridge'` for downstream libraries that embed the runtime via `WHOLE_ARCHIVE` and want runtime `@Native` annotations to resolve against their combined library.

## Caching and invalidation

The builder writes a stamp file into the build directory recording the exact CMake configure arguments. If you change a config value (for example, switching Android STL from `c++_static` to `c++_shared` or changing the ABI), the builder detects the difference and wipes the stale build directory before reconfiguring.

It also declares all files under `sourceDir` with common native extensions (`.c`, `.cpp`, `.h`, `.hpp`, `.cmake`, `CMakeLists.txt`, etc.) as hook dependencies, so changing business code automatically triggers a rebuild.

## Windows MSVC environment details

When `CmakeGenerator.ninja` is used on Windows, the builder must ensure `cl.exe`, `link.exe`, and the Windows SDK paths are available. It does this automatically:

1. Resolve the VS installation root (`vsInstallPath` → `vswhere.exe` → well-known paths).
2. Locate `<vsRoot>\VC\Auxiliary\Build\vcvarsall.bat`.
3. If the current process already has a VS developer environment (`VSCMD_VER` set, or `LIB`/`PATH` containing MSVC paths), reuse it.
4. Otherwise, run `vcvarsall.bat <arch> >nul 2>&1 && set` and capture the environment variables.
5. Pass the captured environment to the CMake configure and build processes.

If `vcvarsall.bat` cannot be found or fails, the builder logs a warning and falls back to the current environment. In that case Ninja may fail to locate `cl.exe`.

## Debugging hook failures

When a hook fails, the Native Assets runner prints the path to a log directory. Look for:

* `stdout.txt` — contains `[dcb]` log lines from `DcbCMakeBuilder`, including the exact CMake command line.
* `stderr.txt` — CMake errors, compiler errors, and `DcbCMakeException` messages.

Common issues:

| Symptom | Likely cause | Fix |
|---|---|---|
| `cmake not found` | CMake not on PATH | Install CMake >= 3.24 and ensure it is on PATH. |
| `vcvarsall.bat not found` | Using Ninja on Windows without MSVC env | Install VS Build Tools, set `vsInstallPath`, or switch to `CmakeGenerator.msbuild`. |
| `libc++_shared.so not found` | Android NDK path wrong / ABI mismatch | Verify `ndkPath`. If `abi` is omitted, check that `input.config.code.targetArchitecture` is supported. |
| `Could not locate built library` | CMake output name mismatch | Ensure `libName` matches `add_library()` in `CMakeLists.txt`. |
| Version mismatch error from `dcb_gen_tool` | `dart_cpp_bridge` and `dcb_gen_tool` versions differ | Run `dart pub upgrade dart_cpp_bridge` or update `dcb_gen_tool`. |

## Further reading

* [Native Assets on dart.dev](https://dart.dev/interop/c-interop#native-assets)
* [Project Directory Structure](/dart_cpp_bridge/v1/guides/fundamentals/project-structure/) — where `hook/build.dart`, `native/`, and generated files live
* [Getting Started](/dart_cpp_bridge/v1/getting-started/) — end-to-end setup from dependencies to first generated call
* [Architecture Design](/dart_cpp_bridge/v1/guides/fundamentals/architecture/) — how the built library is consumed at runtime
