/// 最小对比：@Native (hook 路径) vs DynamicLibrary.open (手动路径)
/// 调用同一个使用 std::mutex 的 C++ 函数。
library;

import 'dart:ffi';
import 'dart:io';

// ============================================================
// 路径 A：@Native（通过 Native Assets hook 构建 + 加载）
// ============================================================

@Native<Int32 Function()>(
  assetId: 'package:mutex_repro/mutex_repro.dart',
  symbol: 'mutex_increment',
)
external int nativeMutexIncrement();

@Native<Int32 Function(Int32, Int32)>(
  assetId: 'package:mutex_repro/mutex_repro.dart',
  symbol: 'plain_add',
)
external int nativePlainAdd(int a, int b);

// ============================================================
// 路径 B：DynamicLibrary.open（手动指定 DLL 路径）
// ============================================================

typedef _MutexIncrementC = Int32 Function();
typedef _MutexIncrementDart = int Function();

typedef _PlainAddC = Int32 Function(Int32, Int32);
typedef _PlainAddDart = int Function(int, int);

class ManualBindings {
  final DynamicLibrary _lib;
  late final _MutexIncrementDart mutexIncrement;
  late final _PlainAddDart plainAdd;

  ManualBindings(String dllPath) : _lib = DynamicLibrary.open(dllPath) {
    mutexIncrement = _lib
        .lookupFunction<_MutexIncrementC, _MutexIncrementDart>(
            'mutex_increment');
    plainAdd =
        _lib.lookupFunction<_PlainAddC, _PlainAddDart>('plain_add');
  }
}
