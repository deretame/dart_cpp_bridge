/// Foreign runtime demo — libuv integrated via ForeignExecutor C API.
///
/// Usage:
/// ```dart
/// import 'package:dcb_foreign_runtime_demo/foreign_runtime_demo.dart';
///
/// await DcbLib.init(libraryPath: '...');
/// await startUvWorker();
/// final result = await askUv(message: 'hello');
/// DcbLib.shutdown();
/// ```
library;

export 'src/native_gen/api/init.dart';
export 'src/native_gen/api/foreign_api.dart';
