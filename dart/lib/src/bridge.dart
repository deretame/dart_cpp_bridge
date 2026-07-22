import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'bindings.dart';
import 'codec.dart';

/// Dart ↔ C++ bridge.
///
/// - **Runtime**: process-wide (shared asio loop / thread pool).
/// - **Session**: per-isolate (own [ReceivePort] so async works on workers).
final class DartCppBridge {
  DartCppBridge._({
    required NativeBindings bindings,
    required int sessionId,
    required ReceivePort receivePort,
  })  : _b = bindings,
        _sessionId = sessionId,
        _rp = receivePort {
    _sub = _rp.listen(_onMessage);
  }

  final NativeBindings _b;
  final int _sessionId;
  final ReceivePort _rp;
  final Map<int, Completer<Uint8List>> _pending = {};
  final Map<int, StreamController<int>> _streams = {};
  int _nextId = 1;
  StreamSubscription<dynamic>? _sub;
  bool _alive = true;

  /// Isolate-local singleton.
  static DartCppBridge? _instance;

  /// Open a session on this isolate (safe to call from any isolate).
  static Future<DartCppBridge> init({String? libraryPath}) async {
    if (_instance != null) return _instance!;

    final b = NativeBindings(NativeBindings.openDefault(path: libraryPath));
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

    final bridge = DartCppBridge._(
      bindings: b,
      sessionId: sessionId,
      receivePort: rp,
    );
    _instance = bridge;
    return bridge;
  }

  void _ensureAlive() {
    if (!_alive) {
      throw StateError('bridge disposed or runtime stopped');
    }
  }

  /// Close this isolate's session. Does not stop the process runtime.
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
    _b.sessionClose(_sessionId);
    _sub?.cancel();
    _rp.close();
    _instance = null;
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
      case MsgType.request:
        break;
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
}
