import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

typedef _InitDartApiC = IntPtr Function(Pointer<Void>);
typedef _InitDartApiD = int Function(Pointer<Void>);

typedef _InitC = Void Function(Int64);
typedef _InitD = void Function(int);

typedef _DisposeC = Void Function();
typedef _DisposeD = void Function();

typedef _InvokeSyncC = Pointer<Uint8> Function(
  Pointer<Uint8>,
  IntPtr,
  Pointer<IntPtr>,
  Pointer<Pointer<Utf8>>,
);
typedef _InvokeSyncD = Pointer<Uint8> Function(
  Pointer<Uint8>,
  int,
  Pointer<IntPtr>,
  Pointer<Pointer<Utf8>>,
);

typedef _InvokeAsyncC = Void Function(Pointer<Uint8>, IntPtr);
typedef _InvokeAsyncD = void Function(Pointer<Uint8>, int);

typedef _StreamCloseC = Void Function(Uint64);
typedef _StreamCloseD = void Function(int);

typedef _FreeC = Void Function(Pointer<Void>);
typedef _FreeD = void Function(Pointer<Void>);

class NativeBindings {
  NativeBindings(DynamicLibrary lib)
      : initDartApi = lib.lookupFunction<_InitDartApiC, _InitDartApiD>('dcb_init_dart_api'),
        init = lib.lookupFunction<_InitC, _InitD>('dcb_init'),
        dispose = lib.lookupFunction<_DisposeC, _DisposeD>('dcb_dispose'),
        shutdown = lib.lookupFunction<_DisposeC, _DisposeD>('dcb_shutdown'),
        invokeSync = lib.lookupFunction<_InvokeSyncC, _InvokeSyncD>('dcb_invoke_sync'),
        invokeAsync = lib.lookupFunction<_InvokeAsyncC, _InvokeAsyncD>('dcb_invoke_async'),
        streamClose = lib.lookupFunction<_StreamCloseC, _StreamCloseD>('dcb_stream_close'),
        free = lib.lookupFunction<_FreeC, _FreeD>('dcb_free');

  final _InitDartApiD initDartApi;
  final _InitD init;
  final _DisposeD dispose;
  final _DisposeD shutdown;
  final _InvokeSyncD invokeSync;
  final _InvokeAsyncD invokeAsync;
  final _StreamCloseD streamClose;
  final _FreeD free;

  static DynamicLibrary openDefault({String? path}) {
    if (path != null) {
      return DynamicLibrary.open(path);
    }
    if (Platform.isWindows) {
      return DynamicLibrary.open('dart_cpp_bridge.dll');
    }
    if (Platform.isLinux) {
      return DynamicLibrary.open('libdart_cpp_bridge.so');
    }
    if (Platform.isMacOS) {
      return DynamicLibrary.open('libdart_cpp_bridge.dylib');
    }
    throw UnsupportedError('unsupported platform');
  }
}
