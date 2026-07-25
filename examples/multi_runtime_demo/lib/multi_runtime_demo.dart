/// Multi-runtime communication demo — generated bindings.
///
/// Usage:
/// ```dart
/// import 'package:dcb_multi_runtime_demo/multi_runtime_demo.dart';
///
/// await DcbLib.init(libraryPath: '...');
/// await startWorkers();
/// final result = await processMessage(message: 'hello');
/// DcbLib.shutdown();
/// ```
library;

export 'src/native_gen/api/init.dart';
export 'src/native_gen/api/multi_runtime_api.dart';
