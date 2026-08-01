/// Dart FFI front-end for the **dart_cpp_bridge** C++ runtime.
///
/// Open a per-isolate session with [DartCppBridge.init], then use
/// [DartCppBridge.invokeSyncMethod] / [DartCppBridge.invokeAsyncMethod] /
/// [DartCppBridge.openStream] to call native methods by id.
/// Full docs: https://github.com/deretame/dart_cpp_bridge
library;

export 'src/bindings.dart';
export 'src/bridge.dart';
export 'src/codec.dart';
