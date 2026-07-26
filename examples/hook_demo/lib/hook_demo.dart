/// Minimal Native Assets demo: native functions compiled by `hook/build.dart`
/// and accessed through `@Native`.
library;

import 'dart:ffi';

@Native<Int32 Function(Int32, Int32)>(
  assetId: 'package:hook_demo/hook_demo.dart',
  symbol: 'hook_demo_add',
)
external int hookDemoAdd(int a, int b);

@Native<Int32 Function(Int32, Int32)>(
  assetId: 'package:hook_demo/hook_demo.dart',
  symbol: 'hook_demo_sub',
)
external int hookDemoSub(int a, int b);

@Native<Int32 Function(Int32, Int32)>(
  assetId: 'package:hook_demo/hook_demo.dart',
  symbol: 'hook_demo_mul',
)
external int hookDemoMul(int a, int b);
