import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

typedef _InitDartApiC = IntPtr Function(Pointer<Void>);
typedef _InitDartApiD = int Function(Pointer<Void>);

typedef _SessionOpenC = Uint64 Function(Int64);
typedef _SessionOpenD = int Function(int);

typedef _SessionCloseC = Void Function(Uint64);
typedef _SessionCloseD = void Function(int);

typedef _ShutdownC = Void Function();
typedef _ShutdownD = void Function();

typedef _InvokeSyncC = Pointer<Uint8> Function(
  Uint64,
  Pointer<Uint8>,
  IntPtr,
  Pointer<IntPtr>,
  Pointer<Pointer<Utf8>>,
);
typedef _InvokeSyncD = Pointer<Uint8> Function(
  int,
  Pointer<Uint8>,
  int,
  Pointer<IntPtr>,
  Pointer<Pointer<Utf8>>,
);

typedef _InvokeAsyncC = Void Function(Uint64, Pointer<Uint8>, IntPtr);
typedef _InvokeAsyncD = void Function(int, Pointer<Uint8>, int);

typedef _StreamCloseC = Void Function(Uint64, Uint64);
typedef _StreamCloseD = void Function(int, int);

typedef _DartFnReplyC = Void Function(
  Uint64,
  Uint64,
  Uint8,
  Pointer<Uint8>,
  IntPtr,
  Pointer<Utf8>,
);
typedef _DartFnReplyD = void Function(
  int,
  int,
  int,
  Pointer<Uint8>,
  int,
  Pointer<Utf8>,
);

typedef _FreeC = Void Function(Pointer<Void>);
typedef _FreeD = void Function(Pointer<Void>);

class NativeBindings {
  NativeBindings(this.lib)
      : initDartApi = lib.lookupFunction<_InitDartApiC, _InitDartApiD>('dcb_init_dart_api'),
        sessionOpen = lib.lookupFunction<_SessionOpenC, _SessionOpenD>('dcb_session_open'),
        sessionClose = lib.lookupFunction<_SessionCloseC, _SessionCloseD>('dcb_session_close'),
        sessionFinalizer = lib.lookup<NativeFunction<Void Function(Pointer<Void>)>>(
          'dcb_session_finalizer',
        ),
        shutdown = lib.lookupFunction<_ShutdownC, _ShutdownD>('dcb_shutdown'),
        invokeSync = lib.lookupFunction<_InvokeSyncC, _InvokeSyncD>('dcb_invoke_sync'),
        invokeAsync = lib.lookupFunction<_InvokeAsyncC, _InvokeAsyncD>('dcb_invoke_async'),
        streamClose = lib.lookupFunction<_StreamCloseC, _StreamCloseD>('dcb_stream_close'),
        dartFnReply = lib.lookupFunction<_DartFnReplyC, _DartFnReplyD>('dcb_dart_fn_reply'),
        free = lib.lookupFunction<_FreeC, _FreeD>('dcb_free');

  final DynamicLibrary lib;
  final _InitDartApiD initDartApi;
  final _SessionOpenD sessionOpen;
  final _SessionCloseD sessionClose;
  final Pointer<NativeFunction<Void Function(Pointer<Void>)>> sessionFinalizer;
  final _ShutdownD shutdown;
  final _InvokeSyncD invokeSync;
  final _InvokeAsyncD invokeAsync;
  final _StreamCloseD streamClose;
  final _DartFnReplyD dartFnReply;
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
