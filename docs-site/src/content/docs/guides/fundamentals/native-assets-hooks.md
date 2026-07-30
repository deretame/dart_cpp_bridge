---
title: Native Assets Build Hooks
description: How dart_cpp_bridge compiles and bundles C++ libraries through Native Assets build hooks
---

`dart_cpp_bridge` uses Dart's **Native Assets** mechanism to build and bundle the C++ library automatically. Instead of manually compiling a DLL / `.so` / `.dylib` and distributing it, you write a small `hook/build.dart` that invokes `DcbCMakeBuilder`. The Dart / Flutter tooling then runs this hook during `dart run`, `flutter run`, or `flutter build`, producing a bundled [CodeAsset] that the generated FFI bindings load at runtime.

## What the hook does

`hook/build.dart` is a normal Dart program executed by the Native Assets pipeline. It receives a `BuildInput` describing the target platform and produces a `BuildOutput` containing declared code assets. For dart_cpp_bridge, the hook:

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

| Parameter | Default | Purpose |
|---|---|---|
| `config` | required | Platform-specific config (`WindowsConfig`, `LinuxConfig`, ...) |
| `assetName` | required | Code asset identifier consumed by generated FFI bindings |
| `assetPackage` | building package | Override the package namespace of the emitted asset |
| `sourceDir` | package root | CMake source directory relative to the package root |
| `libName` | package name | CMake library base name |
| `extraDefines` | `[]` | Extra `-D` flags passed to CMake configure |
| `buildOptions` | `DcbBuildOptions()` | Debug/release override and parallel build toggle |

## Platform configs

Each platform has a dedicated config class controlling generator, compiler, runtime linkage, and cross-compilation settings.

### Windows (`WindowsConfig`)

```dart
WindowsConfig(
  dynamicCrt: true,   // /MD (true) or /MT (false)
  bundleCrt: true,    // copy MSVCP140/VCRUNTIME140 next to the DLL
  architecture: 'x64',  // or 'arm64'
  generator: CmakeGenerator.msbuild, // or CmakeGenerator.ninja
)
```

Key behaviors:

- `dynamicCrt: true` produces a DLL that depends on the MSVC runtime. Set `bundleCrt: true` to copy the correct runtime DLLs next to the output so the app loads the matching version.
- `dynamicCrt: false` links the CRT statically (`/MT`) for a self-contained DLL.
- The builder auto-detects Visual Studio via `vswhere.exe` when neither `vsInstallPath` nor a CMake on PATH is configured.
- `CmakeGenerator.ninja` requires the MSVC environment; the builder invokes `vcvarsall.bat` automatically.

### Linux (`LinuxConfig`)

```dart
LinuxConfig(
  generator: CmakeGenerator.ninja,
  compiler: '/usr/bin/clang++',
  staticLibStdCpp: true, // static-link libstdc++ into the .so
)
```

Key behaviors:

- Defaults to static `libstdc++` linkage so the `.so` does not depend on the target system's `libstdc++.so.6` version.
- `toolchainFile` can be used for cross-compilation; it takes precedence over `compiler`.

### macOS (`MacosConfig`)

```dart
MacosConfig(
  generator: CmakeGenerator.ninja,
  deploymentTarget: '13.0',
  universal: false,
)
```

Key behaviors:

- Single-config generator (Ninja or Unix Makefiles). Build type is set at configure time.
- `deploymentTarget` maps to `CMAKE_OSX_DEPLOYMENT_TARGET`.
- `universal: true` adds both `arm64` and `x86_64` architectures.

### iOS (`IosConfig`)

```dart
IosConfig(
  generator: CmakeGenerator.ninja,
  deploymentTarget: '15.0',
)
```

Key behaviors:

- Cross-compiles with `-DCMAKE_SYSTEM_NAME=iOS`.
- The hooks system tells the builder whether the target is device (`iphoneos`) or simulator (`iphonesimulator`), and the target architecture.
- iOS requires static linking; the builder honors the hooks `linkModePreference`.
- The minimum deployment target is floored at `14.0` because `async_simple` uses `std::atomic::wait/notify`.

### Android (`AndroidConfig`)

```dart
AndroidConfig(
  ndkPath: r'C:\Users\<you>\AppData\Local\Android\Sdk\ndk\29.0.14206865',
  abi: 'arm64-v8a',
  androidPlatform: 21,
  staticStl: true,
)
```

Key behaviors:

- Uses the NDK's CMake toolchain file at `<ndkPath>/build/cmake/android.toolchain.cmake`.
- `staticStl: true` links `c++_static` (self-contained `.so`).
- `staticStl: false` links `c++_shared` and the builder registers `libc++_shared.so` as an additional code asset so it is packaged into the APK.
- On Windows, if `cmake` is not on PATH the builder probes the Visual Studio CMake bundled with the IDE.

## Build options

`DcbBuildOptions` controls behavior across platforms:

```dart
DcbBuildOptions(
  debug: false,     // null = auto-detect from linkingEnabled
  parallel: true, // pass --parallel to cmake --build
)
```

When `debug` is `null`, the builder infers it from `input.config.linkingEnabled`:

- `linkingEnabled == true` → AOT / release → `Release`
- `linkingEnabled == false` → JIT / debug → `Debug`

This matches the Native Assets convention used by `dart run` versus `dart compile` / Flutter release builds.

## Asset name and package

The generated FFI bindings load the native library through a `@Native` annotation like:

```dart
@Native<IntPtr Function()>(assetId: 'package:my_project/src/native_gen/dcb_bindings.dart')
```

`DcbCMakeBuilder` emits a `CodeAsset` whose `name` is the `assetName` you passed, and whose `package` defaults to the building package. The builder also supports `assetPackage: 'dart_cpp_bridge'` for downstream libraries that embed the runtime via `WHOLE_ARCHIVE` and want runtime `@Native` annotations to resolve against their combined library.

## Caching and invalidation

The builder writes a stamp file into the build directory recording the exact CMake configure arguments. If you change a config value (for example, switching Android STL from `c++_static` to `c++_shared`), the builder detects the difference and wipes the stale build directory before reconfiguring.

It also declares all files under `sourceDir` with common native extensions (`.c`, `.cpp`, `.h`, `.hpp`, `.cmake`, `CMakeLists.txt`, etc.) as hook dependencies, so changing business code automatically triggers a rebuild.

## Debugging hook failures

When a hook fails, the Native Assets runner prints the path to a log directory. Look for:

- `stdout.txt` — contains `[dcb]` log lines from `DcbCMakeBuilder`, including the exact CMake command line.
- `stderr.txt` — CMake errors, compiler errors, and `DcbCMakeException` messages.

Common issues:

| Symptom | Likely cause | Fix |
|---|---|---|
| `cmake not found` | CMake not on PATH | Install CMake >= 3.24 and ensure it is on PATH |
| `vcvarsall.bat not found` | Using Ninja on Windows without MSVC env | Install VS Build Tools, or switch to `CmakeGenerator.msbuild` |
| `libc++_shared.so not found` | Android NDK path wrong / ABI mismatch | Verify `ndkPath` and `abi` |
| `Could not locate built library` | CMake output name mismatch | Ensure `libName` matches `add_library()` in CMakeLists.txt |
| Version mismatch error from `dcb_gen_tool` | `dart_cpp_bridge` and `dcb_gen_tool` versions differ | Run `dart pub upgrade dart_cpp_bridge` or update `dcb_gen_tool` |

## Further reading

- [Native Assets on dart.dev](https://dart.dev/interop/c-interop#native-assets)
- [Project Directory Structure](/dart_cpp_bridge/guides/fundamentals/project-structure/) — where `hook/build.dart`, `native/`, and generated files live
- [Getting Started](/dart_cpp_bridge/getting-started/) — end-to-end setup from dependencies to first generated call
- [Architecture Design](/dart_cpp_bridge/guides/fundamentals/architecture/) — how the built library is consumed at runtime
