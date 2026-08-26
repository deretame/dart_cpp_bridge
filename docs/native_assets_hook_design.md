# Native Assets Hook 方案设计

> 本文包含 v1 依赖模型的历史描述（其中部分示例仍写作 async-simple）。
> 当前 v2 依赖和异步模型以 `dart/native/CMakeLists.txt`、
> `docs/versioning.md` 及 `docs/progress-v2.md` 为准。

> 对应阶段：**Phase 3**（见 [progress.md](./progress.md)）
> 状态：**已实现**（随 `dart_cpp_bridge` 1.1.0+ 发布，`hook/build.dart` + `hook/link.dart`；1.2.0 已验证）
> 关联决策：hook 直接驱动 CMake、CMakeLists 为构建单点真相、平台配置用密封类（非 YAML `user_defines`）、Android 用系统 CMake(≥3.25) + NDK toolchain、`cc_builder`/`CBuilder` 不适用
> 平台优先级：**Windows > Linux > Android > macOS**（iOS 暂缓）
> 更新日期：2026-07-26

---

## 1. 背景与目标

### 1.1 现状

当前原生库完全靠手动 CMake 构建：

```powershell
# 1. 下载 Dart API DL 头文件
cmake -P dart/native/cmake/fetch_dart_api.cmake
# 2. 配置基础库依赖（asio / async-simple）
cmake -S dart/native -B dart/native/build
# 3. 构建各 example 动态库
cmake -S examples/codegen_demo -B examples/codegen_demo/build
cmake --build examples/codegen_demo/build --config Release
```

Dart 侧 [`bindings.dart`](../dart/lib/src/bindings.dart) 用裸名 `DynamicLibrary.open('dart_cpp_bridge.dll')`
或显式 `libraryPath` 加载，测试靠 `DCB_LIBRARY_PATH` 环境变量指向手动编出的 DLL。
[`pubspec.yaml`](../dart/pubspec.yaml) 明确标注 “native library built from the monorepo (hooks not ready)”。

### 1.2 目标

用 Dart **Native Assets build hook** 让 `dart run` / `dart test` / `flutter build`
自动编译并打包原生库，免去手动 CMake 步骤：

- **hook 只负责编译 + 链接**，不跑 codegen（codegen 仍是手动前置步骤，与 progress.md 锁定一致）。
- **CMakeLists.txt 保持构建逻辑单点真相**；hook 仅注入平台参数（generator、toolchain、ABI 等），
  不在 Dart 代码里重复实现构建逻辑。
- 平台优先级：**Windows > Linux > Android > macOS**；iOS 暂缓（依赖 link hook，见 §4.6）。

### 1.3 非目标

- 不替换 codegen 工具链（`dcb_gen_tool`）。
- 不改动 wire 协议 / 运行时架构。
- 不做 CI 发布用的预编译产物分发（可作为后续优化，参考官方 `download_asset` 示例）。

---

## 2. Native Assets / Build hooks 现状调研

> 依据 Dart 官方文档（dart.dev/tools/hooks，文档版本 Dart 3.12.2）与 Dart 3.10 发布说明。

### 2.1 基本模型（Dart 3.10+ 已稳定）

- Hook 是放在包 `hook/` 目录下的 Dart 脚本：
  - `hook/build.dart`：**build hook**，编译/下载原生资产（3.10 稳定）。
  - `hook/link.dart`：**link hook**，把多个资产链接到一起（**需 Dart 3.13+**）。
- 入口：`package:hooks/hooks.dart` 的 `build(args, (input, output) async { ... })`。
- 资产类型：`package:code_assets/code_assets.dart` 的 `CodeAsset`（一个动态库）。
- 简单 C 构建可用 `package:native_toolchain_c` 的 `CBuilder`——**本项目不适用**（见 §3.1）。

```dart
// hook/build.dart（官方最小示例）
import 'package:hooks/hooks.dart';
import 'package:native_toolchain_c/native_toolchain_c.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    final name = input.packageName;
    final builder = CBuilder.library(
      name: name,
      assetName: '$name.dart',
      sources: ['src/$name.c'],
    );
    await builder.run(input: input, output: output, logger: /* ... */);
  });
}
```

### 2.2 关键 API 对象

| 对象 | 角色 | 关键字段/方法 |
|------|------|---------------|
| `BuildInput` | 只读输入 | `packageName`、`packageRoot`、`targetOS`、`targetArchitecture`（经 code_assets）、`outputDirectory` / `sharedOutputDirectory`、`userDefines`、`dryRun`、链接信息 |
| `BuildOutputBuilder` | 只写输出 | `assets.code.add(CodeAsset(...))`、`metadata`、`dependencies.add(uri)`（声明依赖文件，变更触发缓存失效） |
| `CodeAsset` | 一个动态库资产 | asset ID 形如 `package:<包名>/<资产名>` |
| `input.userDefines` | 用户自定义参数 | `input.userDefines['key']`、`input.userDefines.path('key')`（相对路径解析） |

- **Asset ID 与 `@Native`**：资产 ID 为 `package:<package-name>/<asset-name>`。
  Dart 侧用 `@Native<...>()` 注解的 `external` 函数会自动按库 URI 解析 asset ID，
  运行时由 SDK 把 `@Native` 引用与 hook 产出的资产关联。
- **自动打包**：`dart run` / `dart build` / `dart test` 会自动跑 hook 并把产物打包进应用。
- **依赖顺序**：build hook 按 pubspec 依赖顺序执行；一个包的 hook 可以消费**直接依赖包**的 hook 产出的
  `assets` / `metadata`；不支持循环依赖。

### 2.3 半密封环境（semi-hermetic）——本方案的关键约束

Hook 运行在**半密封环境**中：`Platform.environment` **不暴露**父进程的全部环境变量，
以保证可复现、可缓存。**只有以下变量被透传**：

| 类别 | 透传变量 | 对本项目的意义 |
|------|----------|----------------|
| 路径/系统根 | `PATH` | **可直接调用 PATH 上的 `cmake` / 编译器** |
| | `HOME`、`USERPROFILE` | 在默认安装位置找工具 |
| | `SYSTEMDRIVE`、`SYSTEMROOT`、`WINDIR` | **Windows 上 CMake 定位 MSVC 所需** |
| | `PROGRAMDATA` | **Windows 上 vswhere.exe 定位 VS 所需** |
| 临时目录 | `TEMP`、`TMP`、`TMPDIR` | 构建临时文件 |
| 代理 | `HTTP_PROXY`、`HTTPS_PROXY`、`NO_PROXY` | FetchContent 走代理 |
| clang | `LIBCLANG_PATH` | （codegen 用，hook 一般不用） |
| Android | `ANDROID_HOME`、`ANDROID_NDK`、`ANDROID_NDK_HOME`、`ANDROID_NDK_LATEST_HOME`、`ANDROID_NDK_ROOT` | **定位 NDK 的 android.toolchain.cmake** |
| Nix | `NIX_*` 前缀 | Nix 环境 |

> 其余变量一律剥离；上述变量变更会触发 hook 缓存失效。

**结论**：
1. `PATH` 透传 → hook 可直接 `Process.run('cmake', ...)`；Windows 上 CMake 的 VS 生成器
   能靠 `PROGRAMDATA`/`SYSTEMROOT` 自动找到 MSVC。
2. Android 靠 `ANDROID_HOME`/`ANDROID_NDK*` 定位 NDK toolchain 文件。
3. 为健壮性，CMake / NDK 路径支持经密封类配置字段显式覆盖（见 §4.2），默认值取上述透传变量。

### 2.4 配置入口：user_defines 与本项目选型

SDK 提供 `user_defines`：在**根包** `pubspec.yaml` 的 `hooks.user_defines.<包名>` 下写键值对，
hook 经 `input.userDefines['key']` / `input.userDefines.path('key')` 读取。它是官方提供的、
绕过半密封环境向 hook 传参的通道，但本质是**无类型的字符串 map**。

**本项目不采用 `user_defines` 作为配置入口**，改用 Dart **密封类（sealed class）**在 hook
调用处按平台配置（详见 §4.2）：每个平台只携带自己相关的字段，`switch` 编译期穷尽校验，
配置即代码、可被静态分析与 IDE 补全。`user_defines` 仅在此记录以备调研参考。

### 2.5 可参考的官方示例

| 示例 | 与本项目的关联 |
|------|----------------|
| `native_dynamic_linking` | 多个相互依赖的原生库——对应“runtime + wire”组合 |
| `use_dart_api` | 使用 Dart VM C API——本项目 `dart_api_dl` 同类场景 |
| `sqlite` / `mini_audio` / `stb_image` | CMake 驱动编译 + 打包的完整范例 |
| `download_asset` | 预编译产物下载分发（后续优化方向） |

---

## 3. 本项目的核心约束与挑战

### 3.1 为什么不能用 CBuilder / cc_builder

`CBuilder` 直接调编译器、不走构建系统，无法表达本项目的构建复杂度：

- **FetchContent** 拉取 asio / async-simple（header-only，但需其 INTERFACE 目标的 include 与编译定义）。
- **`$<LINK_LIBRARY:WHOLE_ARCHIVE,...>`**：`ffi_entry.cpp` 的 `dcb_*` 导出（运行时整体最低要求为 CMake 3.25）
  只被 Dart FFI 引用、C++ 侧零引用，普通 STATIC 链接会被丢弃，必须 WHOLE_ARCHIVE 强制全量链入。
- **C++20 协程**（async-simple `Lazy`）+ 跨平台编译定义（`ASIO_STANDALONE`、`_WIN32_WINNT` 等）。

→ **决策：hook 驱动 CMake，CMakeLists 为单点真相**（与既有记忆决策一致）。

### 3.2 构建单元界定：“一个 DLL = runtime + wire”

生成的 wire 在 **DLL 加载期无条件注册 dispatch**，运行时通过已注册 dispatch 路由请求。
因此 wire 必须与 runtime 编进**同一个动态库**：

```text
最终动态库 = 基础运行时(runtime.cpp / object_handle.cpp / foreign_runtime.cpp / ffi_entry.cpp / dart_api_dl.c)
           + 生成的 wire_dispatch.cpp
           + 用户业务实现(api_impl/*.cpp)
```

这引出**两层 hook** 的设计（§4.1）。

### 3.3 基础库链接模型

[`dart/native/CMakeLists.txt`](../dart/native/CMakeLists.txt) 产出 `dart_cpp_bridge::runtime`
（STATIC 库），下游以 WHOLE_ARCHIVE 链接。依赖版本（Asio、stdexec、MPMCQueue）在此单点钉死并传递。

### 3.4 Dart API 头文件

`dart_api_dl.c/.h` 由 [`dart/native/cmake/fetch_dart_api.cmake`](../dart/native/cmake/fetch_dart_api.cmake) 下载。
如果源码树中不存在这些 gitignored 文件，native CMake 会自动下载到构建目录；
也可以通过 `DCB_DART_API_DIR` 指定已有目录。

### 3.5 库加载迁移

现状 [`bindings.dart`](../dart/lib/src/bindings.dart) 用 `DynamicLibrary.open('dart_cpp_bridge.dll')`
裸名加载，Native Assets 打包后裸名找不到 bundled asset。两条迁移路线（§4.8）：
- **A. `@Native` 注解**（官方推荐，自动解析 asset ID）——需把 `lookupFunction` 重构为 `external @Native`。
- **B. 解析 asset 绝对路径**后 `DynamicLibrary.open(path)`——改动小，但需拿到 bundled 路径。

---

## 4. 方案设计

### 4.1 总体架构：两层 hook

```text
┌─ 基础包 dart_cpp_bridge（dart/）─────────────────────┐
│  hook/build.dart                                      │
│    └─ CMake 编 runtime 源 → 共享库 dart_cpp_bridge.*  │
│       资产 ID: package:dart_cpp_bridge/dart_cpp_bridge.dart
│    用途：dart/ 自身测试；手写 wire 的用户              │
└───────────────────────────────────────────────────────┘
            ▲ 直接依赖（pubspec）
┌─ 下游 codegen 项目（如 examples/codegen_demo）────────┐
│  hook/build.dart                                      │
│    └─ 解析 dart_cpp_bridge/native 路径                │
│       CMake: add_subdirectory(runtime) + 编 wire+impl │
│       WHOLE_ARCHIVE 链接 → 共享库 <项目名>.*          │
│    资产 ID: package:<项目名>/<项目名>.dart            │
└───────────────────────────────────────────────────────┘
```

- **基础包 hook**：编 runtime 共享库（直接编 `ffi_entry.cpp` 等源，`dcb_*` 自然导出，
  此场景无需 WHOLE_ARCHIVE——WHOLE_ARCHIVE 只在“STATIC 库链入下游 SHARED 库”时才需要）。
  供 `dart/` 自身测试与“手写 wire”用户。
- **下游 hook**：复用现有 CMake 模式（`add_subdirectory(dart/native)` + WHOLE_ARCHIVE），
  把 runtime + 生成 wire + 业务实现编成一体共享库。这与
  [`examples/codegen_demo/CMakeLists.txt`](../examples/codegen_demo/CMakeLists.txt) 现有结构一致。

> 说明：基础包纯 runtime 共享库不含 dispatch，无法独立发起业务调用；它的价值在于
> 让 `dart/` 包的 FFI/codec/session 测试摆脱手动构建，并验证 hook→CMake 工具链。

### 4.2 平台配置：密封类层次

**决策：不使用 `user_defines`（YAML）配置，改为在 hook 中以 Dart 密封类（sealed class）按平台配置。**

理由：
- `user_defines` 是字符串键值映射（`input.userDefines['cmake']`），无类型约束、无补全，
  拼写错误只能在运行时暴露；不同平台所需参数差异大（Android 要 NDK/ABI，macOS 要架构），
  用扁平 map 表达会混杂大量“本平台用不到”的键。
- 密封类让**每个平台只携带自己相关的字段**，`switch` 编译期穷尽校验（漏掉平台直接报错），
  配置即代码、可被 IDE 补全与静态分析，下游 hook 作者一目了然。

`dart_cpp_bridge` 在 `lib/hook.dart` 暴露配置层次与构建器：

```dart
/// 平台配置密封类：不同平台对应不同子类，只携带该平台相关字段。
sealed class DcbPlatformConfig {
  const DcbPlatformConfig();
}

/// Windows（最高优先）。cmake 缺省从 PATH 解析；生成器缺省由 CMake 自动选择（VS）。
final class WindowsConfig extends DcbPlatformConfig {
  final String? cmake;      // 显式 cmake 可执行文件路径（可选）
  final String? generator;  // 显式生成器（可选，如 "Ninja"）
  const WindowsConfig({this.cmake, this.generator});
}

/// Linux。
final class LinuxConfig extends DcbPlatformConfig {
  final String? cmake;
  final String? generator;  // 缺省 Ninja / Unix Makefiles
  const LinuxConfig({this.cmake, this.generator});
}

/// Android。NDK 缺省按 ANDROID_NDK_HOME > ANDROID_HOME/ndk/<ver> 解析。
final class AndroidConfig extends DcbPlatformConfig {
  final String? cmake;
  final String? ndkPath;      // 显式 NDK 路径（可选）
  final int androidPlatform;  // ANDROID_PLATFORM，缺省 21
  const AndroidConfig({this.cmake, this.ndkPath, this.androidPlatform = 21});
}

/// macOS。
final class MacosConfig extends DcbPlatformConfig {
  final String? cmake;
  final bool universal;  // true → arm64 + x86_64 通用二进制
  const MacosConfig({this.cmake, this.universal = false});
}

/// iOS（暂缓）：依赖 link hook（Dart 3.13+），本期不实现，仅占位。
final class IosConfig extends DcbPlatformConfig {
  final String? cmake;
  const IosConfig({this.cmake});
}

/// 构建器：消费平台配置，驱动 CMake configure + build + 注册资产。
final class DcbCMakeBuilder {
  final DcbPlatformConfig config;
  const DcbCMakeBuilder({required this.config});
  Future<void> run({required BuildInput input, required BuildOutputBuilder output}) async {
    // 见 §4.3 流程
  }
}
```

下游 hook 在调用处按 `targetOS` 选择对应密封子类（`switch` 表达式穷尽所有平台）：

```dart
// hook/build.dart（下游项目）
import 'dart:io';
import 'package:hooks/hooks.dart';
import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    final config = switch (input.targetOS) {
      TargetOS.windows => const WindowsConfig(),
      TargetOS.linux   => const LinuxConfig(),
      TargetOS.android => AndroidConfig(ndkPath: Platform.environment['ANDROID_NDK_HOME']),
      TargetOS.macos   => const MacosConfig(universal: true),
      TargetOS.ios     => const IosConfig(), // 暂缓：构建器内抛 UnsupportedError
      _ => throw UnsupportedError('unsupported targetOS: ${input.targetOS}'),
    };
    await DcbCMakeBuilder(config: config).run(input: input, output: output);
  });
}
```

> 本地工具路径（cmake / NDK）的默认值来自半密封环境透传的变量（`PATH`、`ANDROID_NDK_HOME`
> 等，见 §2.3）；密封类字段作为**代码内显式覆盖**，取代原 `user_defines` 的 YAML 配置职责。

### 4.3 hook 驱动 CMake 的流程

`DcbCMakeBuilder.run` 内部按配置子类解析 cmake 与平台参数，再走统一的 configure/build/注册流程：

```dart
// DcbCMakeBuilder.run 内部（伪代码）
// 1. 按 config 子类解析 cmake 可执行文件与平台参数（穷尽匹配）
final (cmake, platformArgs) = switch (config) {
  WindowsConfig(:final cmake, :final generator) => (cmake ?? 'cmake', _windowsArgs(generator)),
  LinuxConfig(:final cmake, :final generator)   => (cmake ?? 'cmake', _linuxArgs(generator)),
  AndroidConfig(:final cmake, :final ndkPath, :final androidPlatform) =>
    (cmake ?? 'cmake', _androidArgs(input, ndkPath, androidPlatform)), // 见 §4.4
  MacosConfig(:final cmake, :final universal)   => (cmake ?? 'cmake', _macosArgs(input, universal)),
  IosConfig() => throw UnsupportedError('iOS 暂缓（依赖 link hook）'),
};

// 2. 解析 dart_cpp_bridge 包的 native 目录（下游需要，经 package_config.json，见 §4.7）
// 3. configure + build
final buildDir = Directory('${input.outputDirectory.path}/dcb_build');
await run(cmake, ['-S', '.', '-B', buildDir.path, ...platformArgs]);
await run(cmake, ['--build', buildDir.path, '--config', 'Release']);

// 4. 注册产物为 CodeAsset
final libFile = locateBuiltLibrary(buildDir, input); // 按平台找 .dll/.so/.dylib
output.assets.code.add(CodeAsset(
  package: input.packageName,
  name: '${input.packageName}.dart',
  file: libFile.uri,
));

// 5. 声明依赖文件（源码/头文件变更触发缓存失效）
output.dependencies.add(Uri.file('native/generated/wire_dispatch.cpp'));
// ... 其余源文件
```

> 注：`targetOS` / `targetArchitecture` / `outputDirectory` 等字段的精确 API 名称
> 以实施时锁定的 `hooks` / `code_assets` 版本为准（本节为设计意图，非最终签名）。

### 4.4 Android 交叉编译

依据既有决策：**系统 CMake(≥3.25) + NDK 的 android.toolchain.cmake**，无需 SDK 自带 CMake。

```text
NDK 定位：AndroidConfig.ndkPath > $ANDROID_NDK_HOME > $ANDROID_HOME/ndk/<ver>
toolchain：$NDK/build/cmake/android.toolchain.cmake

cmake -S . -B <build> \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=<armeabi-v7a|arm64-v8a|x86|x86_64> \
  -DANDROID_PLATFORM=android-<N> \
  -DCMAKE_BUILD_TYPE=Release
```

`targetArchitecture` → `ANDROID_ABI` 映射：

| Dart targetArchitecture | ANDROID_ABI |
|--------------------------|-------------|
| `arm64` | `arm64-v8a` |
| `arm` | `armeabi-v7a` |
| `ia32` | `x86` |
| `x64` | `x86_64` |

### 4.5 桌面平台

| 平台 | 生成器 / 策略 | 产物 |
|------|---------------|------|
| Windows | CMake 默认（VS 生成器，靠 `PROGRAMDATA`/vswhere 自动定位 MSVC）或 Ninja | `Release/<name>.dll` |
| Linux | 默认 Makefiles / Ninja（gcc/clang） | `lib<name>.so` |
| macOS | 默认 Makefiles / Xcode；`-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64` 可做 universal | `lib<name>.dylib` |

桌面 `targetArchitecture` → 编译器标志：`x64`/`arm64` 经 CMake 主机工具链或
`CMAKE_OSX_ARCHITECTURES`（macOS）/ MSVC `-A x64/ARM64`（Windows）控制。

### 4.6 iOS（暂缓，依赖 link hook）

> **本期暂缓**：iOS 优先级最低，`IosConfig` 仅占位，`DcbCMakeBuilder` 对其抛 `UnsupportedError`。
> 待 Windows / Linux / Android / macOS 落地后再启动。

iOS 通常要求**静态链接**进 App。路线（后续）：
- build hook 用 CMake iOS toolchain（`-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64`）
  编静态库；
- 经 **link hook（Dart 3.13+）** 把静态库链接进最终产物。
- 需把 SDK 下限从 `^3.10.0` 提升到 `^3.13.0`（见 §6 风险）。

### 4.7 下游 hook 如何找到 dart_cpp_bridge/native

下游 hook 需要基础库的 `native/` 目录来 `add_subdirectory`。解析方式：

1. 读 `.dart_tool/package_config.json`，找 `dart_cpp_bridge` 的 `rootUri`。
2. 拼 `<rootUri>/native` 作为 `DCB_ROOT` 传给 CMake。

```dart
Uri resolveDcbNativeDir(BuildInput input) {
  // package_config.json 位于 input.packageRoot 向上最近的 .dart_tool/
  final cfg = readPackageConfig(input); // 解析 packages[].name/rootUri
  final root = cfg['dart_cpp_bridge'];  // file:///.../dart_cpp_bridge/dart/
  return Uri.parse('$root/native');
}
```

> 基础库以 pub 包发布后，`native/` 目录须随包分发（注意 `.pubignore` 不要排除
> `native/include`、`native/src`、`CMakeLists.txt`、`fetch_dart_api.cmake`）。

### 4.8 库加载迁移方案

| 方案 | 改动量 | 优点 | 缺点 |
|------|--------|------|------|
| **A. `@Native` 注解** | 大（重构 bindings.dart 全部 `lookupFunction` 为 `external @Native`） | 官方推荐；asset ID 自动解析；无需手动管路径 | 需为每个 `dcb_*` 写 `@Native` 签名；函数指针型（finalizer）需特殊处理 |
| **B. 解析 asset 路径** | 小（保留 `NativeBindings`，改 `openDefault` 从 bundled 路径 open） | 复用现有 lookup 代码 | 需在运行时拿到 bundled asset 绝对路径（SDK 提供机制待确认） |

**建议**：先以 **B** 跑通（最小改动验证工具链），稳定后评估迁移到 **A** 以贴合官方范式。

### 4.9 缓存与依赖声明

- 用 `output.dependencies.add(uri)` 声明所有参与编译的源/头文件与 `CMakeLists.txt`，
  使源码变更触发 hook 重跑。
- **FetchContent 的 hermeticity 权衡**：Asio / stdexec / MPMCQueue 由 CMake `FetchContent` 在
  configure 期 `git clone`，依赖网络与 git。半密封环境透传代理变量，但首次构建仍需网络。
  - 选项 1（现状）：保留 FetchContent，接受首构联网；CMake 自身有 `_deps` 缓存。
  - 选项 2（更 hermetic）：vendor 依赖进仓库或改为 `find_package`/CPM 缓存。
  - **建议**：Phase 3 先用选项 1 跑通，hermetic 优化列为后续。
- `dart_api` 头文件：native CMake 缺失时自动下载，也支持关闭自动下载并通过
  `DCB_DART_API_DIR` 指向预取目录。

---

## 5. 分阶段实施计划

> 平台优先级：**Windows > Linux > Android > macOS**；iOS 暂缓。
> 每步都以“端到端跑通对应测试”为验收。

1. **Step 1 — 配置基础设施 + Windows 端到端（最高优先）**
   - 实现密封类配置层次与 `DcbCMakeBuilder`（`package:dart_cpp_bridge/hook.dart`，§4.2）。
   - 基础包 `dart/hook/build.dart`（`WindowsConfig` 分支）：CMake 编 runtime → `dart_cpp_bridge.dll`，注册 CodeAsset。
   - `dart/pubspec.yaml`：加 `hooks` / `code_assets` 依赖。
   - 迁移 `dart/test` 库加载（§4.8 方案 B），`dart test` 无需 `DCB_LIBRARY_PATH` 即通过。
   - 下游 `examples/codegen_demo/hook/build.dart`（Windows）：解析 `dart_cpp_bridge/native`，
     CMake 编 runtime+wire+impl，WHOLE_ARCHIVE 链接。
   - 验收：Windows 上 `cd dart && dart test`（约 82 例）与
     `cd examples/codegen_demo && dart test`（约 62 例）全绿，均无需手动 cmake。

2. **Step 2 — Linux**
   - `LinuxConfig` 分支落地（基础包 + 下游 hook）。
   - 验收：Linux 上两处 `dart test` 全绿。

3. **Step 3 — Android**
   - `AndroidConfig` 分支：NDK 定位 + `android.toolchain.cmake` + ABI 映射（§4.4）。
   - 验收：Flutter Android 示例能构建并运行 codegen_demo 绑定。

4. **Step 4 — macOS**
   - `MacosConfig` 分支；`universal: true` 产出 arm64 + x86_64 通用二进制。
   - 验收：macOS 上两处 `dart test` 全绿。

5. **iOS — 暂缓**
   - 依赖 link hook（Dart 3.13+）与静态链接；本期不实现，`IosConfig` 占位、
     构建器抛 `UnsupportedError`（§4.6）。

6. **产品化**
   - 基础库以 pub 包分发 `native/`；外部项目经 FetchContent/pub 依赖接入；
     `.pubignore` 校验；可选预编译产物分发（`download_asset` 模式）。

---

## 6. 风险与开放问题

| 风险/问题 | 说明 | 缓解 |
|-----------|------|------|
| **SDK 下限** | link hook 需 Dart 3.13+；iOS 静态链接依赖 link hook | Windows/Linux/Android/macOS 用 build hook（3.10）先行；iOS 暂缓，届时再提下限 |
| **CMake/编译器定位健壮性** | 半密封环境仅透传有限变量；用户机器 CMake 可能不在 PATH | 密封类配置字段显式覆盖 + env 默认值（§4.2）+ 友好报错（提示安装/配置路径） |
| **FetchContent 联网** | 首构需 git clone asio/async-simple，破坏 hermetic | 先接受；后续 vendor 或 CPM 缓存 |
| **WHOLE_ARCHIVE 跨平台行为** | Android 静态链入时 WHOLE_ARCHIVE 语义需验证（iOS 暂缓） | Step 3 实测；必要时按平台调整链接策略 |
| **asset 路径加载** | 方案 B 需运行时拿到 bundled asset 绝对路径 | 调研 SDK 提供的资产路径解析 API；否则转方案 A（@Native） |
| **多 ABI / fat 包** | Android 多 ABI、macOS universal 的产物组织 | 按 targetArchitecture 分别构建并注册对应 CodeAsset |
| **缓存粒度** | hook 缓存以包为单位；CMake 增量构建与 hook 缓存的协作 | `dependencies.add` 精确声明；构建目录放 `outputDirectory` 内 |

---

## 7. 参考

- Dart Hooks 官方文档：<https://dart.dev/tools/hooks>
- Dart 3.10 发布说明（Build hooks 稳定）：<https://dart.dev/releases>
- code_assets 包：<https://pub.dev/packages/code_assets>
- hooks 包：<https://pub.dev/packages/hooks>
- native_toolchain_c 包：<https://pub.dev/packages/native_toolchain_c>
- 官方示例（含 `native_dynamic_linking` / `use_dart_api` / `sqlite`）：dart.dev/tools/hooks#example-projects
- 本项目设计全文：[frb_and_cpp_bridge_design.md](./frb_and_cpp_bridge_design.md)
- 进度：[progress.md](./progress.md)；技术债：[known_issues.md](./known_issues.md)
