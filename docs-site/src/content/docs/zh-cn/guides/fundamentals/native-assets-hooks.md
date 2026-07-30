---
title: Native Assets Build Hook
description: dart_cpp_bridge 如何通过 Native Assets build hook 自动编译并打包 C++ 库
---

`dart_cpp_bridge` 使用 Dart 的 **Native Assets** 机制来自动构建和打包 C++ 库。你不需要手动编译 DLL / `.so` / `.dylib` 再分发，只需要写一个 `hook/build.dart`，调用 `DcbCMakeBuilder`。之后 `dart run`、`flutter run` 或 `flutter build` 时，Dart / Flutter 工具链会自动执行这个 hook，生成一个 bundling 的 [CodeAsset]，供生成的 FFI 绑定在运行时加载。

## Hook 做了什么

`hook/build.dart` 是一个由 Native Assets 管线执行的普通 Dart 程序。它收到一个描述目标平台的 `BuildInput`，并产出包含已声明 code assets 的 `BuildOutput`。对于 dart_cpp_bridge 来说，hook 会：

1. 根据 `input.config.code.targetOS` 选择平台配置（`WindowsConfig`、`LinuxConfig`、`MacosConfig`、`IosConfig` 或 `AndroidConfig`）。
2. 用该配置调用 `DcbCMakeBuilder`，传入 CMake `sourceDir`、asset 名和库基础名。
3. 由 builder 完成配置、编译、定位产物，并把 native 库作为 `CodeAsset`  emit 出去。

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

| 参数 | 默认值 | 作用 |
|---|---|---|
| `config` | 必填 | 平台配置（`WindowsConfig`、`LinuxConfig` …） |
| `assetName` | 必填 | 生成 FFI 绑定消费的 code asset 标识符 |
| `assetPackage` | 当前包 | 覆盖 emit 的 asset 包命名空间 |
| `sourceDir` | 包根目录 | 相对包根的 CMake 源码目录 |
| `libName` | 包名 | CMake 库基础名 |
| `extraDefines` | `[]` | 传给 CMake configure 的额外 `-D` 参数 |
| `buildOptions` | `DcbBuildOptions()` | 显式控制 Debug/Release、并行编译 |

## 平台配置

每个平台都有独立配置类，控制生成器、编译器、运行时链接和交叉编译选项。

### Windows (`WindowsConfig`)

```dart
WindowsConfig(
  dynamicCrt: true,   // true=/MD, false=/MT
  bundleCrt: true,    // 把 MSVCP140/VCRUNTIME140 复制到 DLL 旁边
  architecture: 'x64',  // 或 'arm64'
  generator: CmakeGenerator.msbuild, // 或 CmakeGenerator.ninja
)
```

关键行为：

- `dynamicCrt: true` 产出的 DLL 依赖 MSVC 运行时。设 `bundleCrt: true` 可把对应版本的运行时 DLL 复制到产物旁边，避免目标系统加载到不兼容版本。
- `dynamicCrt: false` 会静态链接 CRT（`/MT`），产出自包含 DLL。
- 当没有显式配置 `vsInstallPath` 且 PATH 上没有 cmake 时，builder 会通过 `vswhere.exe` 自动探测 Visual Studio。
- `CmakeGenerator.ninja` 需要 MSVC 环境；builder 会自动调用 `vcvarsall.bat`。

### Linux (`LinuxConfig`)

```dart
LinuxConfig(
  generator: CmakeGenerator.ninja,
  compiler: '/usr/bin/clang++',
  staticLibStdCpp: true, // 把 libstdc++ 静态链接进 .so
)
```

关键行为：

- 默认静态链接 `libstdc++`，使 `.so` 不依赖目标系统的 `libstdc++.so.6` 版本。
- `toolchainFile` 可用于交叉编译；设置时优先级高于 `compiler`。

### macOS (`MacosConfig`)

```dart
MacosConfig(
  generator: CmakeGenerator.ninja,
  deploymentTarget: '13.0',
  universal: false,
)
```

关键行为：

- 使用单配置生成器（Ninja 或 Unix Makefiles），`CMAKE_BUILD_TYPE` 在 configure 时确定。
- `deploymentTarget` 映射到 `CMAKE_OSX_DEPLOYMENT_TARGET`。
- `universal: true` 会同时构建 `arm64` 和 `x86_64`。

### iOS (`IosConfig`)

```dart
IosConfig(
  generator: CmakeGenerator.ninja,
  deploymentTarget: '15.0',
)
```

关键行为：

- 通过 `-DCMAKE_SYSTEM_NAME=iOS` 交叉编译。
- hooks 系统会告诉 builder 当前目标是真机（`iphoneos`）还是模拟器（`iphonesimulator`），以及目标架构。
- iOS 要求静态链接；builder 会遵循 hooks 的 `linkModePreference`。
- 由于 `async_simple` 使用 `std::atomic::wait/notify`，最低部署目标被限制在 `14.0`。

### Android (`AndroidConfig`)

```dart
AndroidConfig(
  ndkPath: r'C:\Users\<你>\AppData\Local\Android\Sdk\ndk\29.0.14206865',
  abi: 'arm64-v8a',
  androidPlatform: 21,
  staticStl: true,
)
```

关键行为：

- 使用 NDK 自带的 CMake toolchain file：`<ndkPath>/build/cmake/android.toolchain.cmake`。
- `staticStl: true` 静态链接 `c++_static`，产出自包含 `.so`。
- `staticStl: false` 动态链接 `c++_shared`，builder 会额外把 `libc++_shared.so` 注册为 code asset，确保被打包进 APK。
- 在 Windows 上，如果 PATH 找不到 cmake，builder 会自动探测 Visual Studio  bundled 的 CMake。

## Build 选项

`DcbBuildOptions` 控制跨平台行为：

```dart
DcbBuildOptions(
  debug: false,     // null 时根据 linkingEnabled 自动推断
  parallel: true, // 传给 cmake --build 的 --parallel
)
```

`debug` 为 `null` 时，builder 根据 `input.config.linkingEnabled` 推断：

- `linkingEnabled == true` → AOT / release → `Release`
- `linkingEnabled == false` → JIT / debug → `Debug`

这符合 Native Assets 在 `dart run` 与 `dart compile` / Flutter release build 下的约定。

## Asset 名与包名

生成的 FFI 绑定通过类似下面的 `@Native` 注解加载 native 库：

```dart
@Native<IntPtr Function()>(assetId: 'package:my_project/src/native_gen/dcb_bindings.dart')
```

`DcbCMakeBuilder` 会 emit 一个 `CodeAsset`，其 `name` 是你传入的 `assetName`，`package` 默认为当前构建包。如果你的下游库通过 `WHOLE_ARCHIVE` 嵌入了 runtime，并希望 runtime 的 `@Native` 注解解析到你合并后的库，可以设 `assetPackage: 'dart_cpp_bridge'`。

## 缓存与失效

builder 会在 build 目录写入一个 stamp 文件，记录当前 CMake configure 参数。如果你修改了配置（例如把 Android STL 从 `c++_static` 改为 `c++_shared`），builder 会检测到差异并清空旧 build 目录再重新配置。

它还会把 `sourceDir` 下所有常见 native 源文件（`.c`、`.cpp`、`.h`、`.hpp`、`.cmake`、`CMakeLists.txt` 等）声明为 hook 依赖，因此修改业务代码会自动触发重编。

## 调试 hook 失败

hook 失败时，Native Assets runner 会打印日志目录路径。查看：

- `stdout.txt` — 包含 `DcbCMakeBuilder` 的 `[dcb]` 日志，以及实际执行的 CMake 命令行。
- `stderr.txt` — CMake 错误、编译器错误和 `DcbCMakeException` 信息。

常见问题：

| 现象 | 可能原因 | 解决 |
|---|---|---|
| `cmake not found` | PATH 上没有 CMake | 安装 CMake >= 3.24 并加到 PATH |
| `vcvarsall.bat not found` | Windows 上用 Ninja 但没配 MSVC 环境 | 安装 VS Build Tools，或改用 `CmakeGenerator.msbuild` |
| `libc++_shared.so not found` | Android NDK 路径或 ABI 不对 | 检查 `ndkPath` 和 `abi` |
| `Could not locate built library` | CMake 输出名不一致 | 确保 `libName` 与 CMakeLists.txt 里的 `add_library()` 一致 |
| `dcb_gen_tool` 报版本不匹配 | `dart_cpp_bridge` 与 `dcb_gen_tool` 版本不一致 | 运行 `dart pub upgrade dart_cpp_bridge` 或升级 `dcb_gen_tool` |

## 延伸阅读

- [Native Assets 官方文档](https://dart.dev/interop/c-interop#native-assets)
- [项目目录结构](/dart_cpp_bridge/zh-cn/guides/fundamentals/project-structure/) — `hook/build.dart`、`native/`、生成产物的位置
- [快速开始](/dart_cpp_bridge/zh-cn/getting-started/) — 从依赖到第一次生成调用的完整流程
- [架构设计](/dart_cpp_bridge/zh-cn/guides/fundamentals/architecture/) — 编译好的库如何在运行时被消费
