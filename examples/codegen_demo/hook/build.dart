import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final config = switch (input.config.code.targetOS) {
      OS.windows => const WindowsConfig(),
      OS.linux => const LinuxConfig(),
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
