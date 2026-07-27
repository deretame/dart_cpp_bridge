# iOS / 静态链接支持设计

> 状态：设计阶段，暂未实现。
> 参考：`native_toolchain_rust-1.0.4+0`（Dart 官方 Rust Native Assets 工具链）

---

## 1. 背景

当前 `DcbCMakeBuilder` 硬编码 `DynamicLoadingBundled()` 作为 link mode，仅支持动态库
（`.dll` / `.so` / `.dylib`）。iOS 禁止运行时动态加载，必须静态链入主二进制。

通过分析 Dart 官方 `native_toolchain_rust` 包的实现，发现 **hooks 系统已经内建了
静态链接支持**——builder 只需遵循 `CodeConfig.linkModePreference` 即可。

---

## 2. 核心机制：linkModePreference

```dart
// hooks 系统通过 CodeConfig 传入：
final CodeConfig(:targetOS, :targetArchitecture, :linkModePreference) = input.config.code;

// Flutter 工具链根据目标平台自动设置：
//   iOS      → LinkModePreference.static
//   macOS    → LinkModePreference.dynamic（或 preferDynamic）
//   Android  → LinkModePreference.dynamic
//   Windows  → LinkModePreference.dynamic
//   Linux    → LinkModePreference.dynamic
```

Builder 的职责：
1. 读取 `linkModePreference`
2. 编译对应产物类型（static → `.a`/`.lib`，dynamic → `.so`/`.dll`/`.dylib`）
3. 注册 asset 时使用匹配的 `LinkMode`

**链接由 Dart 工具链的 link hook 完成，不是 builder 的事。**

---

## 3. native_toolchain_rust 的做法

```dart
// config_mapping.dart
LinkMode get linkMode {
  return switch (linkModePreference) {
    LinkModePreference.dynamic || LinkModePreference.preferDynamic
      => DynamicLoadingBundled(),
    LinkModePreference.static || LinkModePreference.preferStatic
      => StaticLinking(),
    _ => throw UnsupportedError('Unsupported LinkModePreference'),
  };
}

// build_runner.dart — 注册 asset
output.assets.code.add(
  CodeAsset(
    package: input.packageName,
    name: assetName,
    linkMode: linkMode,           // ← 动态 or 静态，由平台决定
    file: path.toUri(binaryFilePath),
  ),
  routing: routing,               // 默认 ToAppBundle()
);
```

Rust builder 不关心"怎么链入"，只关心"编什么类型 + 注册什么 linkMode"。

---

## 4. DcbCMakeBuilder 需要的改动

### 4.1 读取 linkMode

```dart
// run() 方法内
final linkMode = switch (input.config.code.linkModePreference) {
  LinkModePreference.static || LinkModePreference.preferStatic
    => StaticLinking(),
  _ => DynamicLoadingBundled(),
};
```

### 4.2 CMake 构建类型

| linkMode | CMake 参数 | 产物 |
|----------|-----------|------|
| `DynamicLoadingBundled()` | `-DBUILD_SHARED_LIBS=ON` | `.so` / `.dll` / `.dylib` |
| `StaticLinking()` | `-DBUILD_SHARED_LIBS=OFF` | `.a` / `.lib` |

当前 CMakeLists.txt 使用 `add_library(... SHARED)`，需要改为：

```cmake
option(DCB_BUILD_STATIC "Build static library" OFF)
if(DCB_BUILD_STATIC)
  add_library(dcb_codegen_demo STATIC ${SOURCES})
else()
  add_library(dcb_codegen_demo SHARED ${SOURCES})
endif()
```

### 4.3 产物定位

```dart
final libFileName = switch (linkMode) {
  StaticLinking() => targetOS.staticLibFileName(libBaseName),  // .a / .lib
  DynamicLoadingBundled() => targetOS.dylibFileName(libBaseName),  // .so / .dll / .dylib
};
```

### 4.4 Asset 注册

```dart
output.assets.code.add(
  CodeAsset(
    package: packageName,
    name: assetName,
    linkMode: linkMode,  // ← 不再硬编码 DynamicLoadingBundled()
    file: libFile.uri,
  ),
);
```

---

## 5. iOS 特有事项

### 5.1 Target triple

参考 `native_toolchain_rust` 的映射：

| 条件 | Rust triple | CMake 等价 |
|------|-------------|-----------|
| iOS device arm64 | `aarch64-apple-ios` | `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64` |
| iOS simulator arm64 | `aarch64-apple-ios-sim` | `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_SYSROOT=iphonesimulator` |
| iOS simulator x64 | `x86_64-apple-ios` | `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_SYSROOT=iphonesimulator` |

### 5.2 IOSSdk 区分

hooks 系统通过 `iOS.targetSdk` 提供：
- `IOSSdk.iPhoneOS` — 真机
- `IOSSdk.iPhoneSimulator` — 模拟器

Builder 据此选择 sysroot。

### 5.3 C++ 运行时

iOS 使用 libc++（Xcode 自带），默认静态链接。无需额外处理。

### 5.4 代码签名

由 Xcode / Flutter 构建系统自动处理，builder 不需要管。

---

## 6. IosConfig 设计（草案）

```dart
final class IosConfig extends DcbPlatformConfig {
  @override
  final String cmake;

  /// Xcode developer directory path.
  /// Defaults to `xcode-select -p` output.
  final String? developerDir;

  /// Minimum iOS deployment target (e.g. '12.0').
  /// Defaults to Flutter's minimum.
  final String? deploymentTarget;

  final List<String> extraDefines;

  const IosConfig({
    this.cmake = 'cmake',
    this.developerDir,
    this.deploymentTarget,
    this.extraDefines = const [],
  });
}
```

注意：iOS 不需要 `compiler` 字段（Xcode clang 是唯一选择），不需要 `staticStl`
（iOS libc++ 默认静态），不需要 `generator`（Xcode 或 Ninja 均可）。

---

## 7. MacosConfig 也可以受益

macOS 当前也是 `UnsupportedError`。实际上 macOS 支持动态库（`.dylib`），只需：

```dart
case MacosConfig cfg:
  // 跟 Linux 类似，用 clang++ + Ninja/Makefiles
  // 默认 dynamic（linkModePreference 会是 dynamic）
```

macOS 比 iOS 简单得多（无 static 要求、无 simulator 区分），可以在 iOS 之前先实现。

---

## 8. 实施顺序

1. **DcbCMakeBuilder 支持 linkMode**（读 `linkModePreference`，调整 CMake 参数和 asset 注册）
2. **MacosConfig 实现**（clang++ + dynamic，跟 Linux 几乎一样）
3. **IosConfig 实现**（static lib + iOS sysroot + simulator 区分）
4. **codegen 生成 @Native externals**（指向用户包的 asset ID，实现无参 init）

---

## 9. 验证方式

- macOS：需要 macOS 设备/CI（当前无）
- iOS：需要 Xcode + iOS simulator 或真机（当前无）
- 可先实现 linkMode 逻辑，在 Windows/Linux 上验证 static 模式编译出 `.a`/`.lib`
  （虽然桌面平台 linkModePreference 默认是 dynamic，但可以手动测试 static 路径）

---

## 10. 参考

- `native_toolchain_rust-1.0.4+0`：`lib/src/config_mapping.dart`（linkMode 映射）
- `native_toolchain_rust-1.0.4+0`：`lib/src/build_runner.dart`（asset 注册 + routing）
- `code_assets` 包：`CodeConfig.linkModePreference`、`LinkModePreference` 枚举
- `hooks` 包：`CodeAsset`、`StaticLinking()`、`DynamicLoadingBundled()`、`ToAppBundle()`
