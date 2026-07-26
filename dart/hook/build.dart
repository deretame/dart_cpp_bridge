import 'package:code_assets/code_assets.dart';
import 'package:dart_cpp_bridge/hook.dart';
import 'package:hooks/hooks.dart';

/// Build hook for the dart_cpp_bridge runtime.
///
/// Compiles the runtime directly as a shared library (`DCB_BUILD_SHARED=ON`)
/// whose `dcb_*` symbols are exported, and emits it as a bundled code asset
/// (`package:dart_cpp_bridge/dart_cpp_bridge.dart`) consumable via `@Native`.
void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) {
      return;
    }
    final config = switch (input.config.code.targetOS) {
      OS.windows => const WindowsConfig(),
      final os => throw UnsupportedError(
        'dart_cpp_bridge hook only supports Windows in this phase: $os',
      ),
    };
    await DcbCMakeBuilder(
      config: config,
      sourceDir: 'native',
      libName: 'dart_cpp_bridge',
      assetName: 'dart_cpp_bridge.dart',
      extraDefines: const ['-DDCB_BUILD_SHARED=ON'],
    ).run(input: input, output: output);
  });
}
