import 'dart:ffi';
import 'dart:io';

import 'package:dart_cpp_bridge/dart_cpp_bridge.dart';
import 'package:test/test.dart';

/// Smoke test for the @Native binding path.
///
/// The package itself is source-only, so it cannot register a Native Assets
/// code asset. When DCB_LIBRARY_PATH points at a built runtime, however, this
/// test resolves the same C ABI through DynamicLibrary and exercises the real
/// session lifecycle. Downstream packages still cover the @Native asset path.
void main() {
  final libraryPath = Platform.environment['DCB_LIBRARY_PATH'];

  test(
    '@Native runtime session smoke',
    () async {
      final path = libraryPath;
      if (path == null || path.isEmpty) {
        fail('DCB_LIBRARY_PATH must be set for this test');
      }
      final lib = DynamicLibrary.open(path);
      final bindings = NativeBindings(
        initDartApi: lib.lookupFunction<InitDartApiC, InitDartApiD>(
          'dcb_init_dart_api',
        ),
        sessionOpen: lib.lookupFunction<SessionOpenC, SessionOpenD>(
          'dcb_session_open',
        ),
        sessionClose: lib.lookupFunction<SessionCloseC, SessionCloseD>(
          'dcb_session_close',
        ),
        sessionFinalizer: lib
            .lookupFunction<Pointer<Void> Function(), Pointer<Void> Function()>(
              'dcb_session_finalizer_ptr',
            )
            .call()
            .cast<FinalizerFn>(),
        shutdown: lib.lookupFunction<ShutdownC, ShutdownD>('dcb_shutdown'),
        invokeSync: lib.lookupFunction<InvokeSyncC, InvokeSyncD>(
          'dcb_invoke_sync',
        ),
        invokeAsync: lib.lookupFunction<InvokeAsyncC, InvokeAsyncD>(
          'dcb_invoke_async',
        ),
        streamClose: lib.lookupFunction<StreamCloseC, StreamCloseD>(
          'dcb_stream_close',
        ),
        dartFnReply: lib.lookupFunction<DartFnReplyC, DartFnReplyD>(
          'dcb_dart_fn_reply',
        ),
        free: lib.lookupFunction<FreeC, FreeD>('dcb_free'),
        setVerboseErrors: lib.lookupFunction<SetVerboseErrorsC, SetVerboseErrorsD>(
          'dcb_set_verbose_errors',
        ),
        setPoolThreads: lib.lookupFunction<SetPoolThreadsC, SetPoolThreadsD>(
          'dcb_set_pool_threads',
        ),
        dropObject: lib
            .lookupFunction<Pointer<Void> Function(), Pointer<Void> Function()>(
              'dcb_drop_object_ptr',
            )
            .call()
            .cast<FinalizerFn>(),
      );

      final bridge = await DartCppBridge.init(
        bindings: bindings,
        poolThreads: 1,
      );
      expect(DartCppBridge.instance, same(bridge));
      bridge.shutdown();
    },
    skip: libraryPath == null || libraryPath.isEmpty
        ? 'Set DCB_LIBRARY_PATH to a built dart_cpp_bridge library'
        : false,
  );
}
