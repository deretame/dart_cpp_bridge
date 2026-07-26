import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'bindings.dart';
import 'codec.dart';

/// Internal holder that pairs a [StreamController] with the per-item decoder
/// for its generic type. Stored in [DartCppBridge._streams] so stream data
/// frames can be dispatched to the right controller with the right decoder.
final class _StreamSubscription<T> {
  _StreamSubscription(this.controller, this.decodeItem);

  final StreamController<T> controller;
  final T Function(ByteReader) decodeItem;
}

/// Dart ↔ C++ bridge.
///
/// - **Runtime**: process-wide.
/// - **Session**: per-isolate (own reply port).
///
/// Lifecycle (aligned with FRB style):
/// - Call [init] when the isolate starts using the bridge.
/// - **No need to manually [dispose]** in normal use: a [NativeFinalizer]
///   closes the native session when this object becomes unreachable or the
///   isolate shuts down (same idea as FRB's finalizer / isolate cleanup).
/// - Optional [dispose] for prompt cleanup (e.g. tests).
/// - [shutdown] stops the **process-wide** runtime — only call from the main
///   isolate when the app exits, never from short-lived workers.
final class DartCppBridge implements Finalizable {
  DartCppBridge._({
    required NativeBindings bindings,
    required int sessionId,
    required ReceivePort receivePort,
    required Pointer<Uint64> finalizerToken,
  })  : _b = bindings,
        _sessionId = sessionId,
        _rp = receivePort,
        _finalizerToken = finalizerToken {
    _sub = _rp.listen(_onMessage);
    _nativeFinalizer.attach(
      this,
      finalizerToken.cast(),
      detach: this,
      externalSize: 64,
    );
  }

  final NativeBindings _b;
  final int _sessionId;
  final ReceivePort _rp;
  final Pointer<Uint64> _finalizerToken;
  final Map<int, Completer<Uint8List>> _pending = {};
  final Map<int, _StreamSubscription<dynamic>> _streams = {};
  /// FRB-style: callbacks passed into C++ calls, keyed by fn_id for this session.
  /// Values are binary-level callbacks: C++ sends raw arg bytes, we send back
  /// raw result bytes. Codegen wraps typed user closures around this layer.
  final Map<int, FutureOr<Uint8List> Function(Uint8List)> _dartFns = {};
  int _nextId = 1;
  int _nextFnId = 1;
  StreamSubscription<dynamic>? _sub;
  bool _alive = true;
  bool _finalizerDetached = false;

  static DartCppBridge? _instance;
  static NativeFinalizer? _sharedFinalizer;
  static NativeFinalizer get _nativeFinalizer => _sharedFinalizer!;

  /// The bridge instance for this isolate, if [init] has been called.
  static DartCppBridge get instance =>
      _instance ?? (throw StateError('DartCppBridge not initialized'));

  /// Open a session on **this** isolate.
  static Future<DartCppBridge> init({String? libraryPath, int poolThreads = 4}) async {
    if (_instance != null) return _instance!;

    final b = NativeBindings(NativeBindings.openDefault(path: libraryPath));
    _sharedFinalizer ??= NativeFinalizer(b.sessionFinalizer);

    final rc = b.initDartApi(NativeApi.initializeApiDLData);
    if (rc != 0) {
      throw StateError('Dart_InitializeApiDL failed: $rc');
    }

    // Configure pool size before runtime starts (session_open triggers start).
    b.setPoolThreads(poolThreads);

    final rp = ReceivePort();
    final sessionId = b.sessionOpen(rp.sendPort.nativePort);
    if (sessionId == 0) {
      rp.close();
      throw StateError('dcb_session_open failed');
    }

    // Token owned by NativeFinalizer (freed in dcb_session_finalizer),
    // unless dispose() detaches and frees it manually.
    final token = malloc<Uint64>()..value = sessionId;

    final bridge = DartCppBridge._(
      bindings: b,
      sessionId: sessionId,
      receivePort: rp,
      finalizerToken: token,
    );
    _instance = bridge;
    return bridge;
  }

  void _ensureAlive() {
    if (!_alive) {
      throw StateError('bridge disposed or runtime stopped');
    }
  }

  void _detachFinalizer() {
    if (_finalizerDetached) return;
    _finalizerDetached = true;
    _nativeFinalizer.detach(this);
    malloc.free(_finalizerToken);
  }

  /// Promptly close this isolate's session.
  /// Optional — [NativeFinalizer] will close it on GC / isolate shutdown.
  void dispose() {
    if (!_alive) return;
    _alive = false;
    for (final c in _pending.values) {
      if (!c.isCompleted) {
        c.completeError(StateError('bridge disposed'));
      }
    }
    _pending.clear();
    for (final s in _streams.values) {
      if (!s.controller.isClosed) s.controller.close();
    }
    _streams.clear();
    _dartFns.clear();

    _detachFinalizer();
    _b.sessionClose(_sessionId);
    _sub?.cancel();
    _rp.close();
    if (identical(_instance, this)) {
      _instance = null;
    }
  }

  /// Close all sessions and stop the shared runtime (process-wide).
  void shutdown() {
    dispose();
    _b.shutdown();
  }

  /// Enable or disable verbose error messages (default: enabled).
  ///
  /// When enabled, C++ error messages are prefixed with `[function_name] ` to
  /// identify which native function threw. This is a process-wide setting.
  void setVerboseErrors(bool enabled) {
    _b.setVerboseErrors(enabled ? 1 : 0);
  }

  int _allocId() => _nextId++;

  void _onMessage(dynamic msg) {
    late final Uint8List bytes;
    if (msg is Uint8List) {
      bytes = msg;
    } else if (msg is TransferableTypedData) {
      bytes = msg.materialize().asUint8List();
    } else if (msg is List<int>) {
      bytes = Uint8List.fromList(msg);
    } else {
      return;
    }

    final frame = parseFrame(bytes);
    switch (frame.type) {
      case MsgType.responseOk:
        _pending.remove(frame.requestId)?.complete(frame.payload);
      case MsgType.responseErr:
        final c = _pending.remove(frame.requestId);
        final r = ByteReader(frame.payload);
        r.i32();
        c?.completeError(StateError(r.str()));
      case MsgType.streamData:
        final s = _streams[frame.requestId];
        if (s != null && !s.controller.isClosed) {
          s.controller.add(s.decodeItem(ByteReader(frame.payload)));
        }
      case MsgType.streamEnd:
        _streams.remove(frame.requestId)?.controller.close();
      case MsgType.streamErr:
        final s = _streams.remove(frame.requestId);
        if (s != null) {
          final r = ByteReader(frame.payload);
          r.i32();
          s.controller.addError(StateError(r.str()));
          s.controller.close();
        }
      case MsgType.dartFnCall:
        // fire-and-forget async handle; reply goes back via FFI.
        unawaited(_handleDartFnCall(frame));
      case MsgType.request:
        break;
    }
  }

  Future<void> _handleDartFnCall(Frame frame) async {
    final replyId = frame.requestId;
    try {
      final r = ByteReader(frame.payload);
      final fnId = r.u64();
      // Everything after the fn_id is the typed argument payload.
      final argBytes =
          frame.payload.sublist(8); // 8 == sizeof(u64) on the wire
      final fn = _dartFns[fnId];
      if (fn == null) {
        _replyDartFn(replyId, ok: false, error: 'unknown dart fn $fnId');
        return;
      }
      final result = await fn(argBytes);
      _replyDartFn(replyId, ok: true, payload: result);
    } catch (e) {
      _replyDartFn(replyId, ok: false, error: e.toString());
    }
  }

  void _replyDartFn(
    int replyId, {
    required bool ok,
    Uint8List? payload,
    String? error,
  }) {
    final p = payload ?? Uint8List(0);
    final Pointer<Uint8> ptr = p.isEmpty ? nullptr : malloc<Uint8>(p.length);
    if (p.isNotEmpty) {
      ptr.asTypedList(p.length).setAll(0, p);
    }
    final Pointer<Utf8> errPtr = error == null ? nullptr : error.toNativeUtf8();
    try {
      _b.dartFnReply(
        _sessionId,
        replyId,
        ok ? 1 : 0,
        ptr,
        p.length,
        errPtr,
      );
    } finally {
      if (ptr != nullptr) {
        malloc.free(ptr);
      }
      if (errPtr != nullptr) {
        malloc.free(errPtr);
      }
    }
  }

  /// Register a Dart callback for a reverse FFI call from C++.
  ///
  /// [fn] is a binary-level callback: C++ sends the typed argument payload as
  /// raw bytes, and expects raw result bytes back. Codegen wraps typed user
  /// closures into this shape. The returned id must be written into the request
  /// payload so C++ can construct a `DartFn<Ret(Args...)>`.
  int registerDartFn(FutureOr<Uint8List> Function(Uint8List) fn) {
    final id = _nextFnId++;
    _dartFns[id] = fn;
    return id;
  }

  /// Unregister a callback previously registered with [registerDartFn].
  void unregisterDartFn(int id) {
    _dartFns.remove(id);
  }

  /// Open a typed stream from C++.
  ///
  /// [methodId] is the generated wire method id. [payload] is the request
  /// payload excluding the stream id (the stream id is allocated here and used
  /// as [request_id]). [decodeItem] decodes one stream data item from the
  /// payload bytes.
  Stream<T> openStream<T>(
    int methodId,
    Uint8List payload,
    T Function(ByteReader) decodeItem,
  ) {
    _ensureAlive();
    final id = _allocId();
    final controller = StreamController<T>(
      onCancel: () {
        _b.streamClose(_sessionId, id);
        _streams.remove(id);
      },
    );
    _streams[id] = _StreamSubscription<T>(controller, decodeItem);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: methodId,
      payload: payload,
    ));
    return controller.stream;
  }

  /// Invoke an async method that optionally produces stream events.
  ///
  /// Used for C++ functions with `std::optional<StreamSink<T>>` parameters.
  /// The same [request_id] carries both stream events (streamData/streamEnd)
  /// and the final response (responseOk/responseErr).
  ///
  /// [controller] receives stream events if provided; pass null to skip.
  /// The stream_id is appended to [payload] automatically (non-zero if
  /// controller is provided, 0 otherwise).
  /// Returns the raw response payload bytes.
  Future<Uint8List> invokeAsyncMethodWithStream<T>(
    int methodId,
    ByteWriter payload,
    StreamController<T>? controller,
    T Function(ByteReader) decodeItem,
  ) async {
    _ensureAlive();
    final id = _allocId();
    // Write stream_id: non-zero if controller provided, 0 otherwise.
    payload.u64(controller != null ? id : 0);
    if (controller != null) {
      _streams[id] = _StreamSubscription<T>(controller, decodeItem);
    }
    final c = Completer<Uint8List>();
    _pending[id] = c;
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: methodId,
      payload: payload.takeBytes(),
    ));
    try {
      return await c.future;
    } finally {
      _streams.remove(id);
    }
  }

  Uint8List _invokeSyncRaw(Uint8List req) {
    _ensureAlive();
    final ptr = malloc<Uint8>(req.length);
    ptr.asTypedList(req.length).setAll(0, req);
    final outLen = malloc<IntPtr>();
    final errPtr = malloc<Pointer<Utf8>>();
    errPtr.value = nullptr;
    try {
      final out = _b.invokeSync(_sessionId, ptr, req.length, outLen, errPtr);
      if (out == nullptr) {
        final err = errPtr.value;
        final msg = err == nullptr ? 'sync failed' : err.toDartString();
        if (err != nullptr) _b.free(err.cast());
        throw StateError(msg);
      }
      final n = outLen.value;
      final bytes = Uint8List.fromList(out.asTypedList(n));
      _b.free(out.cast());
      return bytes;
    } finally {
      malloc.free(ptr);
      malloc.free(outLen);
      malloc.free(errPtr);
    }
  }

  void _invokeAsyncRaw(Uint8List req) {
    _ensureAlive();
    final ptr = malloc<Uint8>(req.length);
    ptr.asTypedList(req.length).setAll(0, req);
    try {
      _b.invokeAsync(_sessionId, ptr, req.length);
    } finally {
      malloc.free(ptr);
    }
  }

  /// Low-level sync invoke for codegen / custom method ids.
  ///
  /// Returns the response **payload** (not the full frame). Throws [StateError]
  /// on wire error frames.
  Uint8List invokeSyncMethod(int methodId, [Uint8List? payload]) {
    final req = makeFrame(
      type: MsgType.request,
      requestId: 0,
      methodId: methodId,
      payload: payload ?? Uint8List(0),
    );
    final resp = parseFrame(_invokeSyncRaw(req));
    if (resp.type == MsgType.responseErr) {
      final r = ByteReader(resp.payload);
      r.i32();
      throw StateError(r.str());
    }
    if (resp.type != MsgType.responseOk) {
      throw StateError('unexpected sync response ${resp.type}');
    }
    return resp.payload;
  }

  /// Low-level async invoke for codegen / custom method ids.
  ///
  /// Completes with the response **payload** on ok, or errors with [StateError].
  Future<Uint8List> invokeAsyncMethod(int methodId, [Uint8List? payload]) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: methodId,
      payload: payload ?? Uint8List(0),
    ));
    return c.future;
  }

  /// Send raw pre-encoded bytes to the native side and await a response.
  ///
  /// The response is matched by [responseId] (defaults to the allocated id).
  /// For malformed frames where C++ responds with `request_id = 0`, pass
  /// `responseId: 0`.
  Future<Uint8List> invokeRawAsync(Uint8List rawBytes, {int? responseId}) {
    final id = responseId ?? _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    _invokeAsyncRaw(rawBytes);
    return c.future;
  }
}

/// Base class for opaque C++ objects exported to Dart.
///
/// Unified `dispose()` + `NativeFinalizer` attach/detach logic so generated
/// opaque classes don't have to repeat it.
abstract base class CppOpaqueInterface implements Finalizable {
  CppOpaqueInterface({required DartCppBridge bridge, required int handle})
      : _bridge = bridge,
        _handle = handle {
    _finalizer = NativeFinalizer(_bridge._b.dropObject);
    _attachFinalizer();
  }

  final DartCppBridge _bridge;
  final int _handle;
  late final NativeFinalizer _finalizer;
  bool _disposed = false;

  /// The native handle. Exposed so generated wrapper classes can pass handles
  /// as arguments to other generated methods.
  int get handle => _handle;

  /// The bridge instance this object belongs to.
  DartCppBridge get bridge => _bridge;

  /// Public alias of [_ensureAlive] for generated subclasses in other
  /// packages/libraries.
  void ensureAlive() => _ensureAlive();

  void _attachFinalizer() {
    _finalizer.attach(
      this,
      Pointer.fromAddress(_handle).cast<Void>(),
      externalSize: 64,
    );
  }

  void _ensureAlive() {
    if (_disposed) {
      throw StateError('${runtimeType} disposed');
    }
  }

  /// Explicitly drop the native object. Optional — [NativeFinalizer] will drop
  /// it when this Dart object is GC'd or the isolate shuts down.
  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _finalizer.detach(this);
    _bridge._b.dropObject
        .asFunction<void Function(Pointer<Void>)>()(
          Pointer.fromAddress(_handle).cast<Void>(),
        );
  }
}
