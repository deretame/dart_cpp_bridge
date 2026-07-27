import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final cmake = Platform.environment['NIX_DCB_CMAKE'] ??
        (Platform.isWindows
            ? r'C:\Program Files\Microsoft Visual Studio\18\Community'
                r'\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            : 'cmake');
    final config = switch (input.config.code.targetOS) {
      OS.windows => WindowsConfig(cmake: cmake),
      OS.linux => LinuxConfig(cmake: cmake),
      OS.macOS => MacosConfig(cmake: cmake),
      OS.android => AndroidConfig(
          cmake: cmake,
          ndkPath: Platform.environment['ANDROID_NDK_HOME'] ??
              r'C:\Users\windy\AppData\Local\Android\Sdk\ndk\29.0.14206865',
          abi: switch (input.config.code.targetArchitecture) {
            Architecture.arm64 => 'arm64-v8a',
            Architecture.arm => 'armeabi-v7a',
            Architecture.x64 => 'x86_64',
            Architecture.ia32 => 'x86',
            final a => throw UnsupportedError('Unsupported Android arch: $a'),
          },
        ),
      final os => throw UnsupportedError(
          'codegen_demo does not support: $os'),
    };
    await DcbCMakeBuilder(
      config: config,
      assetName: '${input.packageName}.dart',
      libName: 'dcb_codegen_demo',
    ).run(input: input, output: output);
  });
}
