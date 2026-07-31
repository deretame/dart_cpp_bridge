import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final cmake = Platform.environment['NIX_DCB_CMAKE'] ?? 'cmake';
    final config = switch (input.config.code.targetOS) {
      OS.windows => WindowsConfig(
        vsInstallPath: r'C:\Program Files\Microsoft Visual Studio\18\Community',
        cmake: cmake,
        generator: CmakeGenerator.ninja,
      ),
      OS.linux => LinuxConfig(cmake: cmake),
      OS.macOS => MacosConfig(cmake: cmake),
      OS.iOS => IosConfig(cmake: cmake),
      OS.android => AndroidConfig(
        cmake: cmake,
        ndkPath:
            Platform.environment['ANDROID_NDK_HOME'] ??
            r'C:\Users\windy\AppData\Local\Android\Sdk\ndk\29.0.14206865',
        // libuv v1.48.0 requires API 24+ (pthread_barrier_*, getifaddrs, preadv)
        androidPlatform: 24,
      ),
      final os => throw UnsupportedError('codegen_demo does not support: $os'),
    };
    await DcbCMakeBuilder(
      config: config,
      sourceDir: 'native',
      assetName: 'src/native_gen/dcb_bindings.dart',
      libName: 'my_fancy_bridge',
      buildOptions: const DcbBuildOptions(
        copyCompileCommands: true,
        compileCommandsPath: 'build/compile_commands.json',
      ),
    ).run(input: input, output: output);
  });
}
