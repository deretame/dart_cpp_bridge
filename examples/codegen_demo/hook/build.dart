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
      OS.android => const AndroidConfig(
          cmake:
              r'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
          ndkPath:
              r'C:\Users\windy\AppData\Local\Android\Sdk\ndk\29.0.14206865',
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
