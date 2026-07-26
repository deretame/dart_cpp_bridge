import 'dart:ffi';

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

typedef _SetVerboseErrorsC = Void Function(Uint8);
typedef _SetVerboseErrorsD = void Function(int);
typedef _SetPoolThreadsC = Void Function(Uint32);
typedef _SetPoolThreadsD = void Function(int);

/// Asset id of the runtime shared library produced by `hook/build.dart`.
const _kAssetId = 'package:dart_cpp_bridge/dart_cpp_bridge.dart';

// ---------------------------------------------------------------------------
// @Native externals: resolved from the bundled code asset at runtime. This is
// the default binding path (no manual DynamicLibrary.open).
// ---------------------------------------------------------------------------
@Native<_InitDartApiC>(assetId: _kAssetId, symbol: 'dcb_init_dart_api')
external int _dcbInitDartApi(Pointer<Void> data);

@Native<_SessionOpenC>(assetId: _kAssetId, symbol: 'dcb_session_open')
external int _dcbSessionOpen(int replyNativePort);

@Native<_SessionCloseC>(assetId: _kAssetId, symbol: 'dcb_session_close')
external void _dcbSessionClose(int sessionId);

@Native<_ShutdownC>(assetId: _kAssetId, symbol: 'dcb_shutdown')
external void _dcbShutdown();

@Native<_InvokeSyncC>(assetId: _kAssetId, symbol: 'dcb_invoke_sync')
external Pointer<Uint8> _dcbInvokeSync(
  int sessionId,
  Pointer<Uint8> req,
  int reqLen,
  Pointer<IntPtr> outLen,
  Pointer<Pointer<Utf8>> errorOut,
);

@Native<_InvokeAsyncC>(assetId: _kAssetId, symbol: 'dcb_invoke_async')
external void _dcbInvokeAsync(int sessionId, Pointer<Uint8> req, int reqLen);

@Native<_StreamCloseC>(assetId: _kAssetId, symbol: 'dcb_stream_close')
external void _dcbStreamClose(int sessionId, int streamId);

@Native<_DartFnReplyC>(assetId: _kAssetId, symbol: 'dcb_dart_fn_reply')
external void _dcbDartFnReply(
  int sessionId,
  int replyId,
  int ok,
  Pointer<Uint8> payload,
  int payloadLen,
  Pointer<Utf8> errorMsg,
);

@Native<_FreeC>(assetId: _kAssetId, symbol: 'dcb_free')
external void _dcbFree(Pointer<Void> p);

@Native<_SetVerboseErrorsC>(assetId: _kAssetId, symbol: 'dcb_set_verbose_errors')
external void _dcbSetVerboseErrors(int enabled);

@Native<_SetPoolThreadsC>(assetId: _kAssetId, symbol: 'dcb_set_pool_threads')
external void _dcbSetPoolThreads(int n);

// Pointer-returning helpers. NativeFinalizer needs raw function pointers, which
// @Native external functions cannot provide directly, so the runtime exports
// these address getters.
@Native<Pointer<Void> Function()>(
  assetId: _kAssetId,
  symbol: 'dcb_session_finalizer_ptr',
)
external Pointer<Void> _dcbSessionFinalizerPtr();

@Native<Pointer<Void> Function()>(
  assetId: _kAssetId,
  symbol: 'dcb_drop_object_ptr',
)
external Pointer<Void> _dcbDropObjectPtr();

typedef _FinalizerFn = NativeFunction<Void Function(Pointer<Void>)>;

class NativeBindings {
  /// Compat path: resolve symbols from an already-opened [DynamicLibrary].
  ///
  /// Used by downstream demos that build and load their own bridge DLL via
  /// `DartCppBridge.init(libraryPath: ...)`.
  NativeBindings.fromLibrary(DynamicLibrary lib)
      : initDartApi = lib.lookupFunction<_InitDartApiC, _InitDartApiD>('dcb_init_dart_api'),
        sessionOpen = lib.lookupFunction<_SessionOpenC, _SessionOpenD>('dcb_session_open'),
        sessionClose = lib.lookupFunction<_SessionCloseC, _SessionCloseD>('dcb_session_close'),
        sessionFinalizer = lib.lookup<_FinalizerFn>('dcb_session_finalizer'),
        shutdown = lib.lookupFunction<_ShutdownC, _ShutdownD>('dcb_shutdown'),
        invokeSync = lib.lookupFunction<_InvokeSyncC, _InvokeSyncD>('dcb_invoke_sync'),
        invokeAsync = lib.lookupFunction<_InvokeAsyncC, _InvokeAsyncD>('dcb_invoke_async'),
        streamClose = lib.lookupFunction<_StreamCloseC, _StreamCloseD>('dcb_stream_close'),
        dartFnReply = lib.lookupFunction<_DartFnReplyC, _DartFnReplyD>('dcb_dart_fn_reply'),
        free = lib.lookupFunction<_FreeC, _FreeD>('dcb_free'),
        setVerboseErrors = lib.lookupFunction<_SetVerboseErrorsC, _SetVerboseErrorsD>(
          'dcb_set_verbose_errors',
        ),
        setPoolThreads = lib.lookupFunction<_SetPoolThreadsC, _SetPoolThreadsD>(
          'dcb_set_pool_threads',
        ),
        dropObject = lib.lookup<_FinalizerFn>('dcb_drop_object');

  /// Default path: call the runtime through `@Native` externals backed by the
  /// bundled code asset (no manual `DynamicLibrary.open`).
  NativeBindings.native()
      : initDartApi = _dcbInitDartApi,
        sessionOpen = _dcbSessionOpen,
        sessionClose = _dcbSessionClose,
        sessionFinalizer = _dcbSessionFinalizerPtr().cast<_FinalizerFn>(),
        shutdown = _dcbShutdown,
        invokeSync = _dcbInvokeSync,
        invokeAsync = _dcbInvokeAsync,
        streamClose = _dcbStreamClose,
        dartFnReply = _dcbDartFnReply,
        free = _dcbFree,
        setVerboseErrors = _dcbSetVerboseErrors,
        setPoolThreads = _dcbSetPoolThreads,
        dropObject = _dcbDropObjectPtr().cast<_FinalizerFn>();

  final _InitDartApiD initDartApi;
  final _SessionOpenD sessionOpen;
  final _SessionCloseD sessionClose;
  final Pointer<_FinalizerFn> sessionFinalizer;
  final _ShutdownD shutdown;
  final _InvokeSyncD invokeSync;
  final _InvokeAsyncD invokeAsync;
  final _StreamCloseD streamClose;
  final _DartFnReplyD dartFnReply;
  final _FreeD free;
  final _SetVerboseErrorsD setVerboseErrors;
  final _SetPoolThreadsD setPoolThreads;
  final Pointer<_FinalizerFn> dropObject;
}
