import 'package:code_assets/code_assets.dart';
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

  test('Android ABI follows the requested target and rejects unsupported ones', () {
    expect(
      DcbCMakeBuilder.resolveAndroidAbi(Architecture.arm64),
      'arm64-v8a',
    );
    expect(
      DcbCMakeBuilder.resolveAndroidAbi(Architecture.x64),
      'x86_64',
    );
    expect(
      () => DcbCMakeBuilder.resolveAndroidAbi(Architecture.riscv64),
      throwsA(isA<DcbCMakeException>()),
    );
    expect(
      () => DcbCMakeBuilder.resolveAndroidAbi(
        Architecture.arm64,
        'x86_64',
      ),
      throwsA(isA<DcbCMakeException>()),
    );
  });

  test('Windows architecture follows the requested target', () {
    expect(
      DcbCMakeBuilder.resolveWindowsArchitecture(Architecture.arm64),
      'arm64',
    );
    expect(
      DcbCMakeBuilder.resolveWindowsArchitecture(Architecture.x64),
      'x64',
    );
    expect(
      () => DcbCMakeBuilder.resolveWindowsArchitecture(Architecture.arm64, 'x64'),
      throwsA(isA<DcbCMakeException>()),
    );
    expect(
      () => DcbCMakeBuilder.resolveWindowsArchitecture(Architecture.riscv64),
      throwsA(isA<DcbCMakeException>()),
    );
  });
}
