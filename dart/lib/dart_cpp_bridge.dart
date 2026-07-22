/// Dart FFI front-end for the experimental **dart_cpp_bridge** C++ runtime.
///
/// Open a per-isolate session with [DartCppBridge.init], then call demo APIs
/// such as [DartCppBridge.add] / [DartCppBridge.ticks]. Full docs:
/// https://github.com/deretame/dart_cpp_bridge
library;

export 'src/bridge.dart';
export 'src/codec.dart';
