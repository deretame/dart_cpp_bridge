import 'package:dart_cpp_bridge/hook.dart';
import 'package:test/test.dart';

void main() {
  test('DcbCMakeBuilder keeps default CMake arguments enabled', () {
    const builder = DcbCMakeBuilder(
      config: LinuxConfig(),
      assetName: 'bindings.dart',
    );

    expect(builder.useDefaultCmakeArgs, isTrue);
  });

  test('DcbCMakeBuilder can disable default CMake arguments', () {
    const builder = DcbCMakeBuilder(
      config: LinuxConfig(extraDefines: ['-DCUSTOM_TOOLCHAIN=ON']),
      assetName: 'bindings.dart',
      useDefaultCmakeArgs: false,
      extraDefines: ['-DCUSTOM_FEATURE=ON'],
    );

    expect(builder.useDefaultCmakeArgs, isFalse);
    expect(builder.config, isA<LinuxConfig>());
    expect(builder.extraDefines, ['-DCUSTOM_FEATURE=ON']);
  });
}
