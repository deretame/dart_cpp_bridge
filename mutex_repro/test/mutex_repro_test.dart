import 'dart:io';

import 'package:mutex_repro/mutex_repro.dart';
import 'package:test/test.dart';

void main() {
  // =========================================================
  // 路径 A：@Native（hook 构建，Native Assets 加载）
  // =========================================================
  group('@Native (hook path)', () {
    test('plain_add 无 mutex 对照组', () {
      expect(nativePlainAdd(2, 3), 5);
      print('[A] plain_add(2,3) = 5  ✓');
    });

    test('mutex_increment 使用 mutex', () {
      final r1 = nativeMutexIncrement();
      final r2 = nativeMutexIncrement();
      print('[A] mutex_increment() = $r1, $r2  ✓');
      expect(r2, greaterThan(r1));
    });
  });

  // =========================================================
  // 路径 B：DynamicLibrary.open（手动加载同一个 DLL）
  // =========================================================
  group('DynamicLibrary.open (manual path)', () {
    late ManualBindings manual;

    setUpAll(() {
      // DLL 路径：hook 构建产物在 .dart_tool 下面，
      // 也可以手动先 cmake build 然后指向 build/Release/mutex_repro.dll
      final dllPath = _findDll();
      print('[B] 加载 DLL: $dllPath');
      manual = ManualBindings(dllPath);
    });

    test('plain_add 无 mutex 对照组', () {
      expect(manual.plainAdd(2, 3), 5);
      print('[B] plain_add(2,3) = 5  ✓');
    });

    test('mutex_increment 使用 mutex', () {
      final r1 = manual.mutexIncrement();
      final r2 = manual.mutexIncrement();
      print('[B] mutex_increment() = $r1, $r2  ✓');
      expect(r2, greaterThan(r1));
    });
  });
}

/// 寻找 DLL：优先找手动构建的 build/Release，其次找 hook 构建产物。
String _findDll() {
  // 1. 手动构建路径
  final manualBuild = File('build/Release/mutex_repro.dll');
  if (manualBuild.existsSync()) {
    return manualBuild.absolute.path;
  }

  // 2. hook 构建产物（.dart_tool 下）
  final dartTool = Directory('.dart_tool');
  if (dartTool.existsSync()) {
    for (final entity in dartTool.listSync(recursive: true)) {
      if (entity is File && entity.path.endsWith('mutex_repro.dll')) {
        return entity.absolute.path;
      }
    }
  }

  fail('找不到 mutex_repro.dll，请先构建：\n'
      '  cmake -S . -B build\n'
      '  cmake --build build --config Release');
}
