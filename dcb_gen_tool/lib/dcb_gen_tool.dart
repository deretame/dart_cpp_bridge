/// dart_cpp_bridge code generation tool.
///
/// This package provides the `dcb_gen` CLI for generating Dart/C++ bridge
/// code from annotated C++ headers.
///
/// ## Usage
///
/// ```bash
/// # Install globally
/// dart pub global activate dcb_gen_tool
///
/// # Generate bridge code
/// dcb_gen generate dart_cpp_bridge.yaml
/// ```
library dcb_gen_tool;

export 'src/bootstrap.dart' show bootstrap, BootstrapOptions, BootstrapResult;
export 'src/lock_file.dart' show LockFile, ArtifactEntry;
export 'src/package_root.dart' show resolvePackageRoot;
export 'src/platform.dart' show HostPlatform;
