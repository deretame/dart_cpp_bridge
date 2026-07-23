import 'dart:io';

import 'package:codegen_demo/codegen_demo.dart';
import 'package:test/test.dart';

String resolveDemoLibrary() {
  const fromDefine = String.fromEnvironment('DCB_LIBRARY_PATH');
  if (fromDefine.isNotEmpty) return fromDefine;
  final fromEnv = Platform.environment['DCB_LIBRARY_PATH'];
  if (fromEnv != null && fromEnv.isNotEmpty) return fromEnv;

  final roots = [
    Directory.current,
    Directory.current.parent,
    Directory.current.parent.parent,
  ];
  final names = [
    if (Platform.isWindows) ...[
      'build/Release/dcb_codegen_demo.dll',
      'build/Debug/dcb_codegen_demo.dll',
      'build/dcb_codegen_demo.dll',
    ],
    if (Platform.isLinux) 'build/libdcb_codegen_demo.so',
    if (Platform.isMacOS) 'build/libdcb_codegen_demo.dylib',
  ];
  for (final root in roots) {
    for (final rel in names) {
      final f = File(
        '${root.path}${Platform.pathSeparator}${rel.replaceAll('/', Platform.pathSeparator)}',
      );
      if (f.existsSync()) return f.path;
    }
  }
  throw StateError(
    'dcb_codegen_demo library not found. Build examples/codegen_demo first.',
  );
}

void main() {
  setUpAll(() async {
    await initBridge(libraryPath: resolveDemoLibrary());
  });

  tearDownAll(() {
    shutdownBridge();
  });

  test('BRIDGE_SYNC bridge_version', () {
    expect(bridgeVersion(), 42);
  });

  test('BRIDGE_ASYNC add', () async {
    expect(await add(2, 3), 5);
  });

  test('BRIDGE_NORMAL sleep_greeting', () async {
    expect(await sleepGreeting('Ada'), 'hello, Ada');
  });

  test('BRIDGE_ASYNC enum next_status', () async {
    expect(await nextStatus(OrderStatus.created), OrderStatus.paid);
    expect(await nextStatus(OrderStatus.paid), OrderStatus.shipped);
    expect(await nextStatus(OrderStatus.shipped), OrderStatus.created);
  });

  test('BRIDGE_ASYNC optional maybe_double', () async {
    expect(await maybeDouble(null), isNull);
    expect(await maybeDouble(5), 10);
    expect(await maybeDouble(-3), -6);
  });

  test('BRIDGE_ASYNC u32 increment_u32', () async {
    expect(await incrementU32(0), 1);
    expect(await incrementU32(4294967290), 4294967291);
  });

  test('BRIDGE_ASYNC i64 increment_i64', () async {
    expect(await incrementI64(0), 1);
    expect(await incrementI64(9223372036854775800), 9223372036854775801);
    expect(await incrementI64(-9223372036854775800), -9223372036854775799);
  });

  test('BRIDGE_ASYNC bool negate_bool', () async {
    expect(await negateBool(true), false);
    expect(await negateBool(false), true);
  });

  test('BRIDGE_ASYNC optional string', () async {
    expect(await optionalString(null), isNull);
    expect(await optionalString('hello'), 'hello!');
  });

  test('BRIDGE_ASYNC optional enum', () async {
    expect(await optionalStatus(null), isNull);
    expect(await optionalStatus(OrderStatus.created), OrderStatus.paid);
    expect(await optionalStatus(OrderStatus.shipped), OrderStatus.created);
  });
}
