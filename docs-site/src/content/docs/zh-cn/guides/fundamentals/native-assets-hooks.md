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
| `useDefaultCmakeArgs` | `bool` | `true` | 是否注入 builder 自动生成的 CMake configure 参数。设为 `false` 后由项目自己的 `CMakeLists.txt` 或 `extraDefines` 控制这些选项。 |
| `buildOptions` | `DcbBuildOptions` | `DcbBuildOptions()` | 控制 Debug/Release、并行编译、`compile_commands.json`。 |

### 取消 builder 的默认 CMake 参数

如果项目自己的 `CMakeLists.txt` 已经负责生成器、工具链、架构、构建类型和运行时链接选项，可以关闭 builder 的默认参数：

```dart
await DcbCMakeBuilder(
  config: config,
  sourceDir: 'native',
  assetName: 'src/native_gen/dcb_bindings.dart',
  libName: 'my_project',
  useDefaultCmakeArgs: false,
  extraDefines: const [
    '-DMY_PROJECT_FEATURE=ON',
  ],
).run(input: input, output: output);
```

`false` 会跳过 builder 自动生成的 `-G`、架构、工具链、`CMAKE_BUILD_TYPE`、运行时链接、`BUILD_SHARED_LIBS` 和 `CMAKE_EXPORT_COMPILE_COMMANDS` 等 configure 参数；`-S/-B`、后续的 `cmake --build`、`--parallel` 以及你显式提供的 `extraDefines` 仍然会保留。关闭后，`DcbBuildOptions.debug` 不再通过 configure 参数设置构建类型，`copyCompileCommands` 也不会强制 CMake 生成该文件（但如果项目自行生成，builder 仍会尝试复制它）。

## 选择 CMake 集成方式

标准的 Dart/Flutter 项目使用本地 Dart package。只有当 native 部分由一个
自行管理顶层构建的 CMake 工程负责时，才需要使用 `FetchContent` 从 GitHub
获取 bridge 源码。

### 本地 Dart package（默认）

标准流程如下：

1. 在 `pubspec.yaml` 中添加 `dart_cpp_bridge`，然后运行 `dart pub get`。
2. 修改 API 头文件后运行 `dcb_gen_tool generate`。
3. 让 Native Assets hook 构建 native target，或直接配置同一个 CMake 工程。

代码生成时，`dcb_gen_tool` 从 `.dart_tool/package_config.json` 中记录的本地
package 解析 `dart_cpp_bridge/native/include`。默认流程不需要 GitHub checkout，
也不需要声明 `FetchContent`。

### GitHub FetchContent（可选）

如果希望由 CMake 工程自行获取 bridge 源码，可以直接使用仓库根目录提供的
CMake 入口：

```cmake
include(FetchContent)

FetchContent_Declare(
  dart_cpp_bridge
  GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
  GIT_TAG v2.2.0  # 或固定到一个 commit
)
FetchContent_MakeAvailable(dart_cpp_bridge)

# generated/wire_dispatch.cpp 由 dcb_gen_tool 生成。
add_library(my_bridge SHARED
  generated/wire_dispatch.cpp
  api_impl/bridge_api.cpp
)
target_link_libraries(my_bridge PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
```

顶层入口会加入 `dart/native`。默认情况下，该 CMake 项目会获取固定版本的
Asio、stdexec 和 MPMCQueue。如果 Dart API DL 头文件不存在，会下载到构建目录，
不会修改 FetchContent 拉取下来的源码目录。

代码生成仍在 Dart 项目中按平常方式执行；FetchContent 只决定 CMake 从哪里获取
native bridge 及其构建依赖。

### 使用父级 CMake 工程提供的依赖

如果父级工程已经通过包管理器、monorepo 或自己的 `FetchContent` 声明提供
Asio、stdexec 或 MPMCQueue，可以在加入 `dart/native` 之前关闭对应的 fetch 选项：

```cmake
# 在 add_subdirectory(dart_cpp_bridge/dart/native ...) 之前设置。
set(DCB_FETCH_STDEXEC OFF CACHE BOOL "" FORCE)
set(DCB_FETCH_ASIO OFF CACHE BOOL "" FORCE)
set(DCB_FETCH_MPMCQUEUE OFF CACHE BOOL "" FORCE)

# 宿主工程必须在 add_subdirectory 之前提供这些 target：
#   STDEXEC::stdexec
#   asio_iface，或一个导出 asio::asio 的 package
add_subdirectory(path/to/dart_cpp_bridge/dart/native
                 ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)
```

`DCB_FETCH_STDEXEC=OFF` 时，宿主提供的 stdexec target 必须包含 Asio
适配器（`STDEXEC_ENABLE_ASIO=ON`），并且使用与 bridge 一致的 Asio 实现。
`DCB_FETCH_ASIO=OFF` 时，需要提供已有的 `asio_iface` target，或者让
`find_package(asio CONFIG)` 暴露 `asio::asio`。之后 runtime 会直接链接
这些宿主 target，不再自行下载或 patch Asio/stdexec。

这只改变 native 依赖的所有权。`DCB_FETCH_DART_API` 控制 Dart API DL
头文件是否自动下载；关闭后可以通过 `DCB_DART_API_DIR` 指定预取目录。

## 选择 Asio 命名空间

公共 C++ 头文件使用 `DCB_ASIO_NS` 表示 Asio 类型和函数。Runtime 与下游
业务代码会统一使用所选的命名空间：

| CMake 选项 | `DCB_ASIO_NS` | 依赖 |
|---|---|---|
| 默认 | `asio` | standalone Asio |
| `-DDCB_USE_BOOST_ASIO=ON` | `boost::asio` | 由宿主工程或对应的 stdexec 配置提供 Boost.Asio |

业务代码也应使用这个宏，不要把 `asio::` 写死：

```cpp
#include <dart_cpp_bridge/runtime.hpp>

DCB_ASIO_NS::io_context io;
DCB_ASIO_NS::post(io, [] {
  // 在当前选择的 Asio 实现上执行工作
});
```

使用 Boost.Asio 时，设置 `DCB_USE_BOOST_ASIO=ON`，并确保宿主工程提供
匹配的 Boost 头文件/target。在宿主提供 stdexec 的模式下，stdexec 也必须
配置为使用 Boost Asio 适配器。`DCB_ASIO_NS` 是 bridge 支持的命名空间切换
入口；业务代码不应再自行写死另一套 Asio 命名空间。

## CMake 生成器选择

`CmakeGenerator` 选择 CMake 的构建系统生成器。选择依据是目标平台以及宿主是否已把对应工具链放到 PATH 上。

| 生成器 | 值 | 可用平台 | 说明 |
|---|---|---|---|
| `msbuild` | `CmakeGenerator.msbuild` | 仅 Windows | Visual Studio 多配置生成器。不需要额外 PATH 配置，CMake 会自动探测 MSBuild。 |
| `ninja` | `CmakeGenerator.ninja` | 全部 | 单配置生成器，增量编译更快。在 Windows 上，MSVC/clang-cl 构建会自动初始化 MSVC 环境；MSYS2 构建使用自己的 ucrt64 工具链环境。 |
| `makefiles` | `CmakeGenerator.makefiles` | Linux、macOS、iOS | Unix Makefiles 单配置生成器。永远可用，但比 Ninja 慢。 |

- 在 **Windows** 上，`WindowsCompiler.msvc` 且 `generator: null` 时，CMake 通常自动选择 Visual Studio 生成器。`clangCl`、`msys2Clang` 和 `msys2Gcc` 会默认使用 Ninja，避免选中的编译器被静默替换成 MSVC toolset。
- 在 **Windows** 上，`msys2Clang` 和 `msys2Gcc` **只支持 Ninja**。这些 GNU 风格工具链不能使用 `CmakeGenerator.msbuild`；`CmakeGenerator.makefiles` 也不支持 Windows。
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
  compiler: WindowsCompiler.msvc, // msvc | clangCl | msys2Clang | msys2Gcc
  clangClPath: null,        // compiler 为 clangCl 时可指定 clang-cl.exe
  msys2Path: null,          // compiler 为 msys2* 时可指定 MSYS2 根目录
  staticRuntime: true,      // MSYS2 runtime 静态链接
  extraDefines: const [],
)
```

字段说明：

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `cmake` | `String` | `'cmake'` | CMake 可执行文件。可以是绝对路径，也可以是 PATH 上的命令。 |
| `dynamicCrt` | `bool` | `true` | `true` 表示动态链接 MSVC 运行时（`/MD`），`false` 表示静态链接（`/MT`）。 |
| `bundleCrt` | `bool` | `true` | MSVC/clang-cl 的 `dynamicCrt: true` 构建会复制 `MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll`；MSYS2 的 `staticRuntime: false` 构建会复制 `libgcc_s_seh-1.dll`、`libstdc++-6.dll`、`libwinpthread-1.dll`。 |
| `vsInstallPath` | `String?` | `null` | Visual Studio / Build Tools 安装根目录。`null` 时通过 `vswhere.exe` 自动探测，再回退到常见路径。 |
| `architecture` | `String` | `'x64'` | 传给 CMake `-A` 的目标架构。支持 `'x64'`、`'arm64'`。 |
| `generator` | `CmakeGenerator?` | `null` | CMake 生成器。`null` 让 CMake 自动选择（Windows 上通常是 Visual Studio / MSBuild）。 |
| `generatorPath` | `String?` | `null` | 生成器可执行文件的显式路径。`ninja` 对应 `ninja.exe`，`msbuild` 对应 `MSBuild.exe`。 |
| `compiler` | `WindowsCompiler` | `msvc` | `msvc` 使用 `cl.exe`；`clangCl` 使用 MSVC 兼容的 LLVM `clang-cl`；`msys2Clang` / `msys2Gcc` 使用 MSYS2 ucrt64 的 GNU/MinGW 风格 clang / gcc。 |
| `clangClPath` | `String?` | `null` | `compiler: clangCl` 且使用 Ninja 时，可指定 `clang-cl.exe` 的路径；省略时自动探测。使用 Visual Studio 生成器时由 `clangcl` toolset 负责查找。 |
| `msys2Path` | `String?` | `null` | `compiler: msys2Clang` / `msys2Gcc` 时的 MSYS2 根目录。省略时按 `MSYS2_ROOT`、`C:\msys64`、`C:\msys2`、`D:\msys2` 探测。 |
| `staticRuntime` | `bool` | `true` | 仅用于 MSYS2。静态链接 libgcc / libstdc++ / winpthread；设为 `false` 时运行时需要 MSYS2 DLL，且可配合 `bundleCrt` 复制。 |
| `extraDefines` | `List<String>` | `[]` | 传给 CMake configure 的额外 `-D` 参数。 |

关键行为：

- `dynamicCrt: true` 产出的 DLL 依赖 MSVC 运行时。设 `bundleCrt: true` 可把对应版本的运行时 DLL 复制到产物旁边，避免目标系统加载到不兼容版本。
- `dynamicCrt: false` 会静态链接 CRT（`/MT`），产出自包含 DLL。
- 对 `msvc` 和 `clangCl`，`vsInstallPath` 既用于选择 CMake/MSBuild 生成器，也用于在使用 Ninja 时定位 `vcvarsall.bat`。
- `compiler: WindowsCompiler.clangCl` 使用 LLVM 的 MSVC 兼容驱动，同时复用 Visual Studio 的头文件、库和 ABI。使用 Ninja 时，builder 会先调用 `vcvarsall.bat <arch>`，再按显式 `clangClPath`、VS 自带 Clang 工具、PATH、常见 LLVM 安装路径和 LLVM 注册表的顺序查找 `clang-cl.exe`；使用 Visual Studio 生成器时则选择 CMake 的 `clangcl` toolset。
- `compiler: WindowsCompiler.msys2Clang` / `msys2Gcc` 使用 MSYS2 ucrt64 的 GNU/MinGW 风格 clang / gcc，**不需要** `vcvarsall.bat`，但只支持 x64 和 Ninja。builder 会把 `<root>\ucrt64\bin` 加入 CMake 进程的 PATH。
- MSYS2 根目录按 `msys2Path` → `MSYS2_ROOT` → `C:\msys64` → `C:\msys2` → `D:\msys2` 的顺序查找。默认 `staticRuntime: true` 会把 libgcc / libstdc++ / winpthread 静态链接进 DLL；设为 `false` 时需要让 MSYS2 运行时 DLL 位于应用 PATH，或设置 `bundleCrt: true` 将它们复制到 DLL 旁边。`dynamicCrt` 只对 `msvc` / `clangCl` 生效。

#### Windows 编译器示例

如果希望使用 LLVM 的 MSVC 兼容驱动，同时保留 Visual Studio 的头文件、库和
ABI，可以使用 `clangCl`。需要安装 Visual Studio C++ workload（提供头文件、
库和 MSVC STL），并安装 VS 的 “C++ Clang tools for Windows” 组件或独立的
LLVM：

```dart
WindowsConfig(
  compiler: WindowsCompiler.clangCl,
  generator: CmakeGenerator.ninja,
  // Ninja 下可选；自动探测也会查找 VS 自带或已安装的 LLVM。
  clangClPath: r'C:\Program Files\LLVM\bin\clang-cl.exe',
)
```

如果希望使用 GNU/MinGW 风格的 MSYS2 ucrt64 工具链，请安装对应 package
（`mingw-w64-ucrt-x86_64-clang` 或 `mingw-w64-ucrt-x86_64-gcc`），并在自动
探测不到时指定 MSYS2 根目录：

```dart
WindowsConfig(
  compiler: WindowsCompiler.msys2Clang, // 或 WindowsCompiler.msys2Gcc
  generator: CmakeGenerator.ninja,       // MSYS2 只支持 Ninja
  msys2Path: r'D:\msys2',
  staticRuntime: true,                   // 加载时不需要 MSYS2 runtime DLL
)
```

MSYS2 ucrt64 工具链目前只支持 x64。builder 会按
`msys2Path` → `MSYS2_ROOT` → `C:\msys64` → `C:\msys2` → `D:\msys2` 查找，
把 `<root>\ucrt64\bin` 加入 CMake 进程的 PATH，并且不会调用
`vcvarsall.bat`。设 `staticRuntime: false` 时，需要让该目录保留在应用
PATH 中，或设置 `bundleCrt: true`，使 builder 把
`libgcc_s_seh-1.dll`、`libstdc++-6.dll` 和 `libwinpthread-1.dll` 复制到
输出 DLL 旁边。

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
| `cmake not found` | PATH 上没有 CMake | 安装 CMake >= 3.25 并加到 PATH。 |
| `vcvarsall.bat not found` | Windows 上用 Ninja 但没配 MSVC 环境 | 安装 VS Build Tools，设置 `vsInstallPath`，或改用 `CmakeGenerator.msbuild`。 |
| `libc++_shared.so not found` | Android NDK 路径或 ABI 不对 | 检查 `ndkPath`。如果省略 `abi`，检查 `input.config.code.targetArchitecture` 是否受支持。 |
| `Could not locate built library` | CMake 输出名不一致 | 确保 `libName` 与 CMakeLists.txt 里的 `add_library()` 一致。 |
| `dcb_gen_tool` 报版本不匹配 | `dart_cpp_bridge` 与 `dcb_gen_tool` 版本不一致 | 运行 `dart pub upgrade dart_cpp_bridge` 或升级 `dcb_gen_tool`。 |

## 延伸阅读

- [Native Assets 官方文档](https://dart.dev/interop/c-interop#native-assets)
- [项目目录结构](/dart_cpp_bridge/zh-cn/guides/fundamentals/project-structure/) — `hook/build.dart`、`native/`、生成产物的位置
- [快速开始](/dart_cpp_bridge/zh-cn/getting-started/) — 从依赖到第一次生成调用的完整流程
- [架构设计](/dart_cpp_bridge/zh-cn/guides/fundamentals/architecture/) — 编译好的库如何在运行时被消费
