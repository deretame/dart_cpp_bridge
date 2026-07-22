import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

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

/// Sync-only helper for worker isolates that share the process-wide runtime/session.
/// Does **not** call dcb_init (must not steal the owning isolate's reply port).
int invokeBridgeVersionSync({required String libraryPath}) {
  final b = NativeBindings(NativeBindings.openDefault(path: libraryPath));
  final req = _versionRequest();
  final ptr = malloc<Uint8>(req.length);
  ptr.asTypedList(req.length).setAll(0, req);
  final outLen = malloc<IntPtr>();
  final errPtr = malloc<Pointer<Utf8>>();
  errPtr.value = nullptr;
  try {
    final out = b.invokeSync(ptr, req.length, outLen, errPtr);
    if (out == nullptr) {
      final err = errPtr.value;
      final msg = err == nullptr ? 'sync failed' : err.toDartString();
      if (err != nullptr) b.free(err.cast());
      throw StateError(msg);
    }
    final n = outLen.value;
    final bytes = out.asTypedList(n);
    // Parse inline to avoid importing codec cycle issues in minimal helper.
    // Frame: skip to payload after header 4+2+1+1+8+4+4 = 24
    if (n < 28) {
      throw StateError('short sync response');
    }
    final bd = ByteData.sublistView(bytes);
    final plen = bd.getUint32(20, Endian.little);
    if (24 + plen > n || plen < 4) {
      throw StateError('bad sync payload');
    }
    return bd.getInt32(24, Endian.little);
  } finally {
    malloc.free(ptr);
    malloc.free(outLen);
    malloc.free(errPtr);
  }
}

Uint8List _versionRequest() {
  // Minimal DCB1 request for bridgeVersion — duplicated header layout.
  final bd = ByteData(24);
  bd.setUint32(0, 0x31424344, Endian.little); // magic
  bd.setUint16(4, 1, Endian.little); // version
  bd.setUint8(6, 1); // request
  bd.setUint8(7, 0);
  bd.setUint64(8, 0, Endian.little); // request_id
  bd.setUint32(16, 1, Endian.little); // method bridgeVersion
  bd.setUint32(20, 0, Endian.little); // payload_len
  return bd.buffer.asUint8List();
}
