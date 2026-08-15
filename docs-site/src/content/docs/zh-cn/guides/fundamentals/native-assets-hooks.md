---
title: Native Assets Build Hook
description: dart_cpp_bridge 如何通过 Native Assets build hook 自动编译并打包 C++ 库
---

`dart_cpp_bridge` 使用 Dart 的 **Native Assets** 机制来自动构建和打包 C++ 库。你不需要手动编译 DLL / `.so` / `.dylib` 再分发，只需要写一个 `hook/build.dart`，调用 `DcbCMakeBuilder`。之后 `dart run`、`flutter run` 或 `flutter build` 时，Dart / Flutter 工具链会自动执行这个 hook，生成一个 bundling 的 [CodeAsset]，供生成的 FFI 绑定在运行时加载。

## Hook 做了什么

`hook/build.dart` 是一个由 Native Assets 管线执行的普通 Dart 程序。它收到一个描述目标平台的 `BuildInput`，并产出包含已声明 code assets 的 `BuildOutput`。对于 dart_cpp_bridge 来说，hook 会：

1. 根据 `input.config.code.targetOS` 选择平台配置（`WindowsConfig`、`LinuxConfig`、`MacosConfig`、`IosConfig` 或 `AndroidConfig`）。
2. 用该配置调用 `DcbCMakeBuilder`，传入 CMake `sourceDir`、asset 名和库基础名。
3. 由 builder 完成配置、编译、定位产物，并把 native 库作为 `CodeAsset` emit 出去。

Dart / Flutter 会负责缓存、失效判定，并把最终库打包进应用或包中。

## 最简 hook 示例

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
          ndkPath: r'C:\Users\<你>\AppData\Local\Android\Sdk\ndk\<版本>',
        ),
      final os => throw UnsupportedError('不支持的目标平台: $os'),
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

把它保存为项目根目录的 `hook/build.dart`。`sourceDir` 指向包含 `CMakeLists.txt` 的目录（通常是 `native/`）。`assetName` 是生成的 FFI 绑定中 `@Native(assetId: 'package:<包名>/<assetName>')` 使用的标识符。`libName` 是 CMake 输出名（例如 `my_project.dll`、`libmy_project.so`）。

## DcbCMakeBuilder

`DcbCMakeBuilder` 是 `package:dart_cpp_bridge/hook.dart` 暴露的可复用 CMake builder。它在 `input.outputDirectory/dcb_build/` 里执行三步：

1. **配置** — `cmake -S <sourceDir> -B <buildDir> [generator] [defines]`
2. **编译** — `cmake --build <buildDir> --config <Release|Debug> [--parallel]`
3. **Emit** — 定位产物并把 native 库作为 `CodeAsset` 加入 `output.assets.code`

构造参数：

| 参数 | 类型 | 默认值 | 作用 |
|---|---|---|---|
| `config` | `DcbPlatformConfig` | 必填 | 平台配置（`WindowsConfig`、`LinuxConfig`、`MacosConfig`、`IosConfig`、`AndroidConfig`）。 |
| `assetName` | `String` | 必填 | 生成 FFI 绑定消费的 code asset 标识符。 |
| `assetPackage` | `String?` | 当前构建包 | 覆盖 emit 的 asset 包命名空间。通过 `WHOLE_ARCHIVE` 嵌入 runtime 时可设为 `'dart_cpp_bridge'`。 |
| `sourceDir` | `String?` | 包根目录 | 相对包根的 CMake 源码目录。通常是 `'native'`。 |
| `libName` | `String?` | 包名 | CMake 库基础名。必须和 `CMakeLists.txt` 里的 `add_library(<name> ...)` 一致。 |
| `extraDefines` | `List<String>` | `[]` | 传给 CMake configure 的额外 `-D` 参数。全平台生效。 |
| `buildOptions` | `DcbBuildOptions` | `DcbBuildOptions()` | 控制 Debug/Release、并行编译、`compile_commands.json`。 |

## CMake 生成器选择

`CmakeGenerator` 选择 CMake 的构建系统生成器。选择依据是目标平台以及宿主是否已把对应工具链放到 PATH 上。

| 生成器 | 值 | 可用平台 | 说明 |
|---|---|---|---|
| `msbuild` | `CmakeGenerator.msbuild` | 仅 Windows | Visual Studio 多配置生成器。不需要额外 PATH 配置，CMake 会自动探测 MSBuild。 |
| `ninja` | `CmakeGenerator.ninja` | 全部 | 单配置生成器，增量编译更快。在 Windows 上需要 MSVC 环境；builder 会自动调用 `vcvarsall.bat`。 |
| `makefiles` | `CmakeGenerator.makefiles` | Linux、macOS、iOS | Unix Makefiles 单配置生成器。永远可用，但比 Ninja 慢。 |

- 在 **Windows** 上，`generator` 为 `null` 时 CMake 通常自动选择 Visual Studio 生成器。想要更快的增量编译可显式设为 `ninja`。
- 在 **Linux / macOS / iOS** 上，`generator` 为 `null` 时 CMake 通常选择 Unix Makefiles。如果 PATH 上有 `ninja`，建议设为 `ninja`。
- 在 **Android** 上，默认就是 `ninja`，因为 NDK 自带 Ninja。

如果生成器不在 PATH 上，可以通过 `generatorPath` 传绝对路径（例如 `ninja.exe` 或 `MSBuild.exe`）。

## 平台配置

每个平台都有独立配置类，控制生成器、编译器、运行时链接和交叉编译选项。

### Windows（`WindowsConfig`）

```dart
WindowsConfig(
  cmake: 'cmake',
  dynamicCrt: true,      // true = /MD, false = /MT
  bundleCrt: true,      // 把 MSVCP140 / VCRUNTIME140 复制到 DLL 旁边
  vsInstallPath: r'C:\Program Files\Microsoft Visual Studio\2022\Community',
  architecture: 'x64',    // 'x64' 或 'arm64'
  generator: CmakeGenerator.ninja,
  generatorPath: null,
  extraDefines: const [],
)
```

字段说明：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake 可执行文件。可以是绝对路径，也可以是 PATH 上的命令。 |
| `dynamicCrt` | `bool` | `true` | `true` 表示动态链接 MSVC 运行时（`/MD`），`false` 表示静态链接（`/MT`）。 |
| `bundleCrt` | `bool` | `true` | 将对应版本的 CRT DLL（`MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll`）复制到产物旁边。仅在 `dynamicCrt` 为 `true` 时生效。 |
| `vsInstallPath` | `String?` | `null` | Visual Studio / Build Tools 安装根目录。`null` 时通过 `vswhere.exe` 自动探测，再回退到常见路径。 |
| `architecture` | `String` | `'x64'` | 传给 CMake `-A` 的目标架构。支持 `'x64'`、`'arm64'`。 |
| `generator` | `CmakeGenerator?` | `null` | CMake 生成器。`null` 让 CMake 自动选择（Windows 上通常是 Visual Studio / MSBuild）。 |
| `generatorPath` | `String?` | `null` | 生成器可执行文件的显式路径。`ninja` 对应 `ninja.exe`，`msbuild` 对应 `MSBuild.exe`。 |
| `extraDefines` | `List<String>` | `[]` | 传给 CMake configure 的额外 `-D` 参数。 |

关键行为：

- `dynamicCrt: true` 产出的 DLL 依赖 MSVC 运行时。设 `bundleCrt: true` 可把对应版本的运行时 DLL 复制到产物旁边，避免目标系统加载到不兼容版本。
- `dynamicCrt: false` 会静态链接 CRT（`/MT`），产出自包含 DLL。
- `vsInstallPath` 既用于选择 CMake/MSBuild 生成器，也用于在使用 Ninja 时定位 `vcvarsall.bat`。
- `CmakeGenerator.ninja` 需要 MSVC 环境。builder 会自动调用 `vcvarsall.bat <arch>`，但仅当当前进程还没有 VS 开发者环境时（即没有 `VSCMD_VER`，且 `LIB`/`PATH` 里也没有 MSVC 路径）。

### Linux（`LinuxConfig`）

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

字段说明：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake 可执行文件。 |
| `generator` | `CmakeGenerator?` | `null` | `ninja` 或 `makefiles`。`null` 让 CMake 自动选择（通常是 Unix Makefiles）。 |
| `generatorPath` | `String?` | `null` | `ninja` 或 `make` 的显式路径，用于不在 PATH 上的情况。 |
| `compiler` | `String?` | `null` | C++ 编译器可执行文件（例如 `/usr/bin/clang++`）。传给 `-DCMAKE_CXX_COMPILER=<path>`。 |
| `toolchainFile` | `String?` | `null` | 交叉编译用的 CMake toolchain file。传给 `-DCMAKE_TOOLCHAIN_FILE=<path>`。优先级高于 `compiler`。 |
| `staticLibStdCpp` | `bool` | `true` | 把 `libstdc++` 静态链接进输出 `.so`，使其不依赖目标系统的 `libstdc++.so.6` 版本。 |
| `extraDefines` | `List<String>` | `[]` | 传给 CMake 的额外 `-D` 参数。 |

关键行为：

- 默认静态链接 `libstdc++`，产出自包含 `.so`。
- `toolchainFile` 可用于交叉编译；设置时优先级高于 `compiler`。

### macOS（`MacosConfig`）

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

字段说明：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake 可执行文件。 |
| `generator` | `CmakeGenerator?` | `null` | `ninja` 或 `makefiles`。`null` 让 CMake 自动选择。 |
| `generatorPath` | `String?` | `null` | `ninja` 或 `make` 的显式路径。 |
| `compiler` | `String?` | `null` | C++ 编译器可执行文件。`null` 使用 AppleClang。传给 `-DCMAKE_CXX_COMPILER=<path>`。 |
| `deploymentTarget` | `String?` | `null` | 最低 macOS 部署目标（例如 `'13.0'`）。映射到 `CMAKE_OSX_DEPLOYMENT_TARGET`。 |
| `universal` | `bool` | `false` | 构建同时包含 `arm64` 和 `x86_64` 的通用二进制。设置 `CMAKE_OSX_ARCHITECTURES=arm64;x86_64`。 |
| `extraDefines` | `List<String>` | `[]` | 额外 `-D` 参数。 |

关键行为：

- 使用单配置生成器（Ninja 或 Unix Makefiles）。`CMAKE_BUILD_TYPE` 在 configure 时确定。
- `deploymentTarget` 映射到 `CMAKE_OSX_DEPLOYMENT_TARGET`。
- `universal: true` 会同时构建 `arm64` 和 `x86_64`。

### iOS（`IosConfig`）

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

字段说明：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake 可执行文件。 |
| `generator` | `CmakeGenerator?` | `null` | `ninja` 或 `makefiles`。 |
| `generatorPath` | `String?` | `null` | `ninja` 或 `make` 的显式路径。 |
| `developerDir` | `String?` | `null` | Xcode developer 目录。设置时通过 `DEVELOPER_DIR` 环境变量传递。`null` 使用 `xcode-select -p` 的结果。 |
| `deploymentTarget` | `String?` | `null` | 最低 iOS 部署目标覆盖（例如 `'15.0'`）。`null` 使用 `input.config.code.iOS.targetVersion`。 |
| `extraDefines` | `List<String>` | `[]` | 额外 `-D` 参数。 |

关键行为：

- 通过 `-DCMAKE_SYSTEM_NAME=iOS` 交叉编译。
- hooks 系统会告诉 builder 当前目标是真机（`iphoneos`）还是模拟器（`iphonesimulator`），以及目标架构。
- iOS 要求静态链接；builder 会遵循 hooks 的 `linkModePreference`。
- 由于 C++20 Runtime 使用 `std::atomic::wait/notify`，最低部署目标被限制在 `14.0`。

### Android（`AndroidConfig`）

```dart
AndroidConfig(
  cmake: 'cmake',
  ndkPath: r'C:\Users\<你>\AppData\Local\Android\Sdk\ndk\29.0.14206865',
  abi: null,              // null = 根据 targetArchitecture 自动推导
  androidPlatform: 21,
  staticStl: true,
  generator: CmakeGenerator.ninja,
  extraDefines: const [],
)
```

字段说明：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake 可执行文件。在 Windows 上如果不在 PATH，builder 会探测 VS 自带的 CMake。 |
| `ndkPath` | `String` | 必填 | Android NDK 根目录。builder 从 `<ndkPath>/build/cmake/android.toolchain.cmake` 推导 toolchain file。 |
| `abi` | `String?` | `null` | 目标 ABI。`null` 时根据 `input.config.code.targetArchitecture` 自动推导：`arm64` → `arm64-v8a`，`arm` → `armeabi-v7a`，`x64` → `x86_64`，`ia32` → `x86`。 |
| `androidPlatform` | `int` | `21` | 最低 Android API level。传给 `-DANDROID_PLATFORM=android-<level>`。 |
| `staticStl` | `bool` | `true` | `true` 链接 `c++_static`；`false` 链接 `c++_shared` 并额外把 `libc++_shared.so` 注册为 code asset。 |
| `generator` | `CmakeGenerator?` | `CmakeGenerator.ninja` | CMake 生成器。NDK 自带 Ninja，所以默认使用 Ninja。 |
| `extraDefines` | `List<String>` | `[]` | 额外 `-D` 参数。 |

关键行为：

- 使用 NDK 自带的 CMake toolchain file：`<ndkPath>/build/cmake/android.toolchain.cmake`。
- `staticStl: true` 静态链接 `c++_static`，产出自包含 `.so`。
- `staticStl: false` 动态链接 `c++_shared`，builder 会额外把 `libc++_shared.so` 注册为 code asset，确保被打包进 APK。
- 在 Windows 上，如果 PATH 找不到 cmake，builder 会自动探测 Visual Studio 自带的 CMake。如果在 Windows 上使用 Ninja，builder 还会确保 bundled 的 `ninja.exe` 在 PATH 上。

## Build 选项

`DcbBuildOptions` 控制跨平台行为：

```dart
DcbBuildOptions(
  debug: null,                         // null 时根据 linkingEnabled 自动推断
  parallel: true,                      // 传给 cmake --build 的 --parallel
  copyCompileCommands: true,           // 生成并复制 compile_commands.json
  compileCommandsPath: 'compile_commands.json', // 相对包根目录
)
```

字段说明：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `debug` | `bool?` | `null` | 强制 Debug（`true`）或 Release（`false`）。`null` 时根据 `input.config.linkingEnabled` 推断。 |
| `parallel` | `bool` | `true` | 给 `cmake --build` 传入 `--parallel`，允许多文件并行编译。 |
| `copyCompileCommands` | `bool` | `true` | 传给 CMake `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`，并在编译成功后复制生成的 `compile_commands.json`。 |
| `compileCommandsPath` | `String` | `'compile_commands.json'` | `compile_commands.json` 复制到包根目录下的相对路径。父目录会自动创建。 |

`debug` 为 `null` 时，builder 根据 `input.config.linkingEnabled` 推断：

- `linkingEnabled == true` → AOT / release → `Release`
- `linkingEnabled == false` → JIT / debug → `Debug`

这符合 Native Assets 在 `dart run` 与 `dart compile` / Flutter release build 下的约定。

### compile_commands.json

默认情况下，builder 会：

1. 给 CMake 传入 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`（对支持的生成器，如 Ninja 或 Makefiles）。
2. 编译成功后，把生成的 `compile_commands.json` 复制到包根目录（`pubspec.yaml` 旁边）。

如果想把文件放到别处，可设置 `compileCommandsPath` 为相对路径：

```dart
const DcbBuildOptions(
  copyCompileCommands: true,
  compileCommandsPath: 'build/compile_commands.json',
)
```

目标路径相对包根目录解析，缺失的父目录会自动创建。设 `copyCompileCommands: false` 可完全禁用生成和复制。

## Asset 名与包名

生成的 FFI 绑定通过类似下面的 `@Native` 注解加载 native 库：

```dart
@Native<IntPtr Function()>(assetId: 'package:my_project/src/native_gen/dcb_bindings.dart')
```

`DcbCMakeBuilder` 会 emit 一个 `CodeAsset`，其 `name` 是你传入的 `assetName`，`package` 默认为当前构建包。如果你的下游库通过 `WHOLE_ARCHIVE` 嵌入了 runtime，并希望 runtime 的 `@Native` 注解解析到你合并后的库，可以设 `assetPackage: 'dart_cpp_bridge'`。

## 缓存与失效

builder 会在 build 目录写入一个 stamp 文件，记录当前 CMake configure 参数。如果你修改了配置（例如把 Android STL 从 `c++_static` 改为 `c++_shared`，或修改 ABI），builder 会检测到差异并清空旧 build 目录再重新配置。

它还会把 `sourceDir` 下所有常见 native 源文件（`.c`、`.cpp`、`.h`、`.hpp`、`.cmake`、`CMakeLists.txt` 等）声明为 hook 依赖，因此修改业务代码会自动触发重编。

## Windows MSVC 环境细节

在 Windows 上使用 `CmakeGenerator.ninja` 时，builder 必须保证 `cl.exe`、`link.exe` 和 Windows SDK 路径可用。它会自动完成：

1. 解析 VS 安装根目录（`vsInstallPath` → `vswhere.exe` → 常见路径）。
2. 定位 `<vsRoot>\VC\Auxiliary\Build\vcvarsall.bat`。
3. 如果当前进程已经有 VS 开发者环境（设置了 `VSCMD_VER`，或 `LIB`/`PATH` 里已有 MSVC 路径），则复用该环境。
4. 否则执行 `vcvarsall.bat <arch> >nul 2>&1 && set` 并捕获环境变量。
5. 把捕获到的环境传给 CMake configure 和 build 进程。

如果找不到 `vcvarsall.bat` 或执行失败，builder 会记录 warning 并回退到当前环境。此时 Ninja 可能无法找到 `cl.exe`。

## 调试 hook 失败

hook 失败时，Native Assets runner 会打印日志目录路径。查看：

- `stdout.txt` — 包含 `DcbCMakeBuilder` 的 `[dcb]` 日志，以及实际执行的 CMake 命令行。
- `stderr.txt` — CMake 错误、编译器错误和 `DcbCMakeException` 信息。

常见问题：

| 现象 | 可能原因 | 解决 |
|---|---|---|
| `cmake not found` | PATH 上没有 CMake | 安装 CMake >= 3.24 并加到 PATH。 |
| `vcvarsall.bat not found` | Windows 上用 Ninja 但没配 MSVC 环境 | 安装 VS Build Tools，设置 `vsInstallPath`，或改用 `CmakeGenerator.msbuild`。 |
| `libc++_shared.so not found` | Android NDK 路径或 ABI 不对 | 检查 `ndkPath`。如果省略 `abi`，检查 `input.config.code.targetArchitecture` 是否受支持。 |
| `Could not locate built library` | CMake 输出名不一致 | 确保 `libName` 与 CMakeLists.txt 里的 `add_library()` 一致。 |
| `dcb_gen_tool` 报版本不匹配 | `dart_cpp_bridge` 与 `dcb_gen_tool` 版本不一致 | 运行 `dart pub upgrade dart_cpp_bridge` 或升级 `dcb_gen_tool`。 |

## 延伸阅读

- [Native Assets 官方文档](https://dart.dev/interop/c-interop#native-assets)
- [项目目录结构](/dart_cpp_bridge/zh-cn/guides/fundamentals/project-structure/) — `hook/build.dart`、`native/`、生成产物的位置
- [快速开始](/dart_cpp_bridge/zh-cn/getting-started/) — 从依赖到第一次生成调用的完整流程
- [架构设计](/dart_cpp_bridge/zh-cn/guides/fundamentals/architecture/) — 编译好的库如何在运行时被消费
