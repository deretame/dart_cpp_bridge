import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'bindings.dart';
import 'codec.dart';

/// Phase-1 hand-written bridge facade (long-lived reply port + request_id).
final class DartCppBridge {
  DartCppBridge._(this._b);

  final NativeBindings _b;
  final ReceivePort _rp = ReceivePort();
  final Map<int, Completer<Uint8List>> _pending = {};
  final Map<int, StreamController<int>> _streams = {};
  int _nextId = 1;
  StreamSubscription<dynamic>? _sub;

  static DartCppBridge? _instance;

  static Future<DartCppBridge> init({String? libraryPath}) async {
    if (_instance != null) return _instance!;
    final b = NativeBindings(NativeBindings.openDefault(path: libraryPath));
    final rc = b.initDartApi(NativeApi.initializeApiDLData);
    if (rc != 0) {
      throw StateError('Dart_InitializeApiDL failed: $rc');
    }
    final bridge = DartCppBridge._(b);
    bridge._start();
    _instance = bridge;
    return bridge;
  }

  void _start() {
    _sub = _rp.listen(_onMessage);
    _b.init(_rp.sendPort.nativePort);
  }

  void dispose() {
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
    _b.dispose();
    _sub?.cancel();
    _rp.close();
    _instance = null;
  }

  void shutdown() {
    dispose();
    _b.shutdown();
  }

  int _allocId() => _nextId++;

  void _onMessage(dynamic msg) {
    Uint8List bytes;
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
        final c = _pending.remove(frame.requestId);
        c?.complete(frame.payload);
      case MsgType.responseErr:
        final c = _pending.remove(frame.requestId);
        final r = ByteReader(frame.payload);
        r.i32();
        final m = r.str();
        c?.completeError(StateError(m));
      case MsgType.streamData:
        final s = _streams[frame.requestId];
        if (s != null && !s.isClosed) {
          s.add(ByteReader(frame.payload).i32());
        }
      case MsgType.streamEnd:
        final s = _streams.remove(frame.requestId);
        s?.close();
      case MsgType.streamErr:
        final s = _streams.remove(frame.requestId);
        if (s != null) {
          final r = ByteReader(frame.payload);
          r.i32();
          final m = r.str();
          s.addError(StateError(m));
          s.close();
        }
      case MsgType.request:
        break;
    }
  }

  Uint8List _invokeSyncRaw(Uint8List req) {
    final ptr = malloc<Uint8>(req.length);
    ptr.asTypedList(req.length).setAll(0, req);
    final outLen = malloc<IntPtr>();
    final errPtr = malloc<Pointer<Utf8>>();
    errPtr.value = nullptr;
    try {
      final out = _b.invokeSync(ptr, req.length, outLen, errPtr);
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
    final ptr = malloc<Uint8>(req.length);
    ptr.asTypedList(req.length).setAll(0, req);
    try {
      _b.invokeAsync(ptr, req.length);
    } finally {
      malloc.free(ptr);
    }
  }

  // ---- demo APIs ----

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
    final req = makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.add.value,
      payload: payload.takeBytes(),
    );
    _invokeAsyncRaw(req);
    final body = await c.future;
    return ByteReader(body).i32();
  }

  Future<String> sleepTest() async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final req = makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.sleepTest.value,
    );
    _invokeAsyncRaw(req);
    final body = await c.future;
    return ByteReader(body).str();
  }

  Stream<int> ticks({int count = 5, int intervalMs = 100}) {
    final id = _allocId();
    final controller = StreamController<int>(
      onCancel: () {
        _b.streamClose(id);
        _streams.remove(id);
      },
    );
    _streams[id] = controller;

    final payload = ByteWriter()
      ..i32(count)
      ..i32(intervalMs);
    final req = makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.ticks.value,
      payload: payload.takeBytes(),
    );
    _invokeAsyncRaw(req);
    return controller.stream;
  }
}
