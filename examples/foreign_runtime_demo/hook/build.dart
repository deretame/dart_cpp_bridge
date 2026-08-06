import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final config = switch (input.config.code.targetOS) {
      OS.windows => WindowsConfig(),
      OS.linux => LinuxConfig(),
      OS.macOS => MacosConfig(),
      OS.iOS => IosConfig(),
      final os => throw UnsupportedError(
        'foreign_runtime_demo does not support: $os',
      ),
    };
    await DcbCMakeBuilder(
      config: config,
      sourceDir: 'native',
      assetName: 'src/native_gen/dcb_bindings.dart',
      libName: 'dcb_foreign_runtime_demo',
    ).run(input: input, output: output);
  });
}
