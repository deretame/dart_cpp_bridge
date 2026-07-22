import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'bindings.dart';
import 'codec.dart';

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
  final Map<int, StreamController<int>> _streams = {};
  /// FRB-style: callbacks passed into C++ calls, keyed by fn_id for this session.
  final Map<int, FutureOr<String> Function(String)> _dartFns = {};
  int _nextId = 1;
  int _nextFnId = 1;
  StreamSubscription<dynamic>? _sub;
  bool _alive = true;
  bool _finalizerDetached = false;

  static DartCppBridge? _instance;
  static NativeFinalizer? _sharedFinalizer;
  static NativeFinalizer get _nativeFinalizer => _sharedFinalizer!;

  /// Open a session on **this** isolate.
  static Future<DartCppBridge> init({String? libraryPath}) async {
    if (_instance != null) return _instance!;

    final b = NativeBindings(NativeBindings.openDefault(path: libraryPath));
    _sharedFinalizer ??= NativeFinalizer(b.sessionFinalizer);

    final rc = b.initDartApi(NativeApi.initializeApiDLData);
    if (rc != 0) {
      throw StateError('Dart_InitializeApiDL failed: $rc');
    }

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
      if (!s.isClosed) s.close();
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
        if (s != null && !s.isClosed) {
          s.add(ByteReader(frame.payload).i32());
        }
      case MsgType.streamEnd:
        _streams.remove(frame.requestId)?.close();
      case MsgType.streamErr:
        final s = _streams.remove(frame.requestId);
        if (s != null) {
          final r = ByteReader(frame.payload);
          r.i32();
          s.addError(StateError(r.str()));
          s.close();
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
      final arg = r.str();
      final fn = _dartFns[fnId];
      if (fn == null) {
        _replyDartFn(replyId, ok: false, error: 'unknown dart fn $fnId');
        return;
      }
      final result = await fn(arg);
      final payload = ByteWriter()..str(result);
      _replyDartFn(replyId, ok: true, payload: payload.takeBytes());
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

  int _registerDartFn(FutureOr<String> Function(String) fn) {
    final id = _nextFnId++;
    _dartFns[id] = fn;
    return id;
  }

  void _unregisterDartFn(int id) {
    _dartFns.remove(id);
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

  int bridgeVersion() {
    final req = makeFrame(
      type: MsgType.request,
      requestId: 0,
      methodId: MethodId.bridgeVersion.value,
    );
    final resp = parseFrame(_invokeSyncRaw(req));
    if (resp.type != MsgType.responseOk) {
      throw StateError('unexpected sync response');
    }
    return ByteReader(resp.payload).i32();
  }

  Future<int> add(int a, int b) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()
      ..i32(a)
      ..i32(b);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.add.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).i32();
  }

  Future<String> sleepTest() async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.sleepTest.value,
    ));
    return ByteReader(await c.future).str();
  }

  Stream<int> ticks({int count = 5, int intervalMs = 100}) {
    final id = _allocId();
    final controller = StreamController<int>(
      onCancel: () {
        _b.streamClose(_sessionId, id);
        _streams.remove(id);
      },
    );
    _streams[id] = controller;
    final payload = ByteWriter()
      ..i32(count)
      ..i32(intervalMs);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.ticks.value,
      payload: payload.takeBytes(),
    ));
    return controller.stream;
  }

  Future<String> echo(String s) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..str(s);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.echo.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).str();
  }

  Future<void> failAsync([String message = 'fail_async']) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..str(message);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.failAsync.value,
      payload: payload.takeBytes(),
    ));
    await c.future;
  }

  Stream<int> failStream([String message = 'fail_stream']) {
    final id = _allocId();
    final controller = StreamController<int>(
      onCancel: () {
        _b.streamClose(_sessionId, id);
        _streams.remove(id);
      },
    );
    _streams[id] = controller;
    final payload = ByteWriter()..str(message);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.failStream.value,
      payload: payload.takeBytes(),
    ));
    return controller.stream;
  }

  void invokeSyncNonSyncMethodForTest() {
    _invokeSyncRaw(makeFrame(
      type: MsgType.request,
      requestId: 0,
      methodId: MethodId.add.value,
    ));
  }

  Future<void> invokeUnknownMethodForTest() async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: 0x7ffffffe,
    ));
    await c.future;
  }

  Future<void> invokeBadFrameForTest() async {
    final c = Completer<Uint8List>();
    _pending[0] = c;
    _invokeAsyncRaw(Uint8List.fromList([1, 2, 3, 4, 5]));
    await c.future;
  }

  /// FRB-style reverse call (C++ **async** wait on io via co_await).
  ///
  /// ```dart
  /// final s = await bridge.callDartHello((name) => 'Hello, $name!');
  /// ```
  ///
  /// [dartCallback] may be sync or `async` on the Dart side.
  Future<String> callDartHello(FutureOr<String> Function(String name) dartCallback) {
    return _callDartHelloImpl(dartCallback, MethodId.callDartHello);
  }

  /// FRB-style reverse call (C++ **sync** block on current native thread).
  ///
  /// Library does **not** move this off the io thread. If C++ calls sync on io,
  /// the scheduler stalls until Dart replies — caller's responsibility.
  Future<String> callDartHelloSync(FutureOr<String> Function(String name) dartCallback) {
    return _callDartHelloImpl(dartCallback, MethodId.callDartHelloSync);
  }

  Future<String> _callDartHelloImpl(
    FutureOr<String> Function(String name) dartCallback,
    MethodId method,
  ) async {
    final fnId = _registerDartFn(dartCallback);
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..u64(fnId);
    try {
      _invokeAsyncRaw(makeFrame(
        type: MsgType.request,
        requestId: id,
        methodId: method.value,
        payload: payload.takeBytes(),
      ));
      return ByteReader(await c.future).str();
    } finally {
      _unregisterDartFn(fnId);
    }
  }
}
