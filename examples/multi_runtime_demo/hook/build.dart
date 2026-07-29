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
      OS.windows => WindowsConfig(cmake: cmake),
      OS.linux => LinuxConfig(cmake: cmake),
      OS.macOS => MacosConfig(cmake: cmake),
      OS.iOS => IosConfig(cmake: cmake),
      final os => throw UnsupportedError(
          'multi_runtime_demo does not support: $os'),
    };
    await DcbCMakeBuilder(
      config: config,
      assetName: 'src/native_gen/dcb_bindings.dart',
      libName: 'dcb_multi_runtime_demo',
    ).run(input: input, output: output);
  });
}
