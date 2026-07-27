import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final config = switch (input.config.code.targetOS) {
      OS.windows => const WindowsConfig(
          // Explicit parameters for testing:
          dynamicCrt: true, // /MD
          bundleCrt: true, // copy CRT DLLs next to output
          vsInstallPath:
              r'C:\Program Files\Microsoft Visual Studio\18\Community',
          architecture: 'x64',
          generator: CmakeGenerator.msbuild,
        ),
      final os => throw UnsupportedError('mutex_repro only supports Windows: $os'),
    };
    await DcbCMakeBuilder(
      config: config,
      assetName: '${input.packageName}.dart',
      buildOptions: const DcbBuildOptions(debug: false),
    ).run(input: input, output: output);
  });
}
