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

  /// The bridge instance for this isolate, if [init] has been called.
  static DartCppBridge get instance =>
      _instance ?? (throw StateError('DartCppBridge not initialized'));

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

  /// Sync demo: returns the native bridge protocol version (`i32`).
  int bridgeVersion() {
    return ByteReader(invokeSyncMethod(MethodId.bridgeVersion.value)).i32();
  }

  /// Async demo: `a + b` computed on the C++ side (`Lazy`).
  Future<int> add(int a, int b) async {
    final payload = ByteWriter()
      ..i32(a)
      ..i32(b);
    return ByteReader(await invokeAsyncMethod(MethodId.add.value, payload.takeBytes()))
        .i32();
  }

  /// Normal-channel demo: sleeps on a worker pool, then returns a done string.
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

  /// Stream demo: emits `0 .. count-1` with optional delay between items.
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

  /// Async demo: echoes [s] back from C++.
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

  /// Async demo: doubles [input] if non-null, returns null otherwise.
  Future<int?> maybeDouble(int? input) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeOptI32(input);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.maybeDouble.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).readOptI32();
  }

  /// Async demo: sums a list of ints on the C++ side.
  Future<int> sumList(List<int> values) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeListI32(values);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.sumVec.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).i32();
  }

  /// Async demo: reverses a Uint8List on the C++ side.
  Future<Uint8List> reverseBytes(Uint8List input) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeUint8List(input);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.reverseBytes.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).readUint8List();
  }

  /// Async demo: returns the next status code on the C++ side.
  Future<StatusCode> nextStatus(StatusCode current) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeEnum(current.index);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.nextStatus.value,
      payload: payload.takeBytes(),
    ));
    return StatusCode.values[ByteReader(await c.future).readEnum()];
  }

  /// Async demo: sums a fixed-length array of 4 ints on the C++ side.
  Future<int> sumFixedFour(List<int> values) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeFixedArrayI32(values);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.sumFixedFour.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).i32();
  }

  /// Async demo: greets a person from a Dart class on the C++ side.
  Future<String> greet(Person person) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()
      ..str(person.name)
      ..i32(person.age);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.greet.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).str();
  }

  /// Async demo: sums scores from a Map on the C++ side.
  Future<int> scoreTotal(Map<String, int> scores) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeMapStringToI32(scores);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.scoreTotal.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).i32();
  }

  /// Async demo: sums values from a Set on the C++ side.
  Future<int> setSum(Set<int> values) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeSetI32(values);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.setSum.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).i32();
  }

  /// Async demo: echoes a (int, String) pair.
  Future<(int, String)> pairEcho((int, String) input) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writePairIntString(input);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.pairEcho.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).readPairIntString();
  }

  /// Async demo: echoes a (int, String, bool) tuple.
  Future<(int, String, bool)> tupleEcho((int, String, bool) input) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeTupleIntStringBool(input);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.tupleEcho.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).readTupleIntStringBool();
  }

  /// Async demo: echoes a signed 128-bit integer back as BigInt.
  Future<BigInt> echoI128(BigInt value) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..writeI128(value);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.nextI128.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).readI128();
  }

  /// Async demo: sums the ages of a list of people on the C++ side.
  Future<int> totalAges(List<Person> people) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..u32(people.length);
    for (final p in people) {
      payload.str(p.name);
      payload.i32(p.age);
    }
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.totalAges.value,
      payload: payload.takeBytes(),
    ));
    return ByteReader(await c.future).i32();
  }

  /// Async demo: create an opaque Counter object on the C++ side.
  Future<Counter> createCounter({int initialValue = 0}) async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter()..i32(initialValue);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.counterCreate.value,
      payload: payload.takeBytes(),
    ));
    final handle = ByteReader(await c.future).u64();
    return Counter._(bridge: this, handle: handle);
  }

  /// Default constructor: create a Counter with initial value 0.
  Future<Counter> createCounterDefault() async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.counterCreateDefault.value,
    ));
    final handle = ByteReader(await c.future).u64();
    return Counter._(bridge: this, handle: handle);
  }

  /// Factory constructor as a static method: create a Counter with value 0.
  Future<Counter> createCounterZero() async {
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.counterZero.value,
    ));
    final handle = ByteReader(await c.future).u64();
    return Counter._(bridge: this, handle: handle);
  }

  Future<void> _counterIncrement(int handle, int delta) async {
    final payload = ByteWriter()
      ..u64(handle)
      ..i32(delta);
    await invokeAsyncMethod(MethodId.counterIncrement.value, payload.takeBytes());
  }

  Future<int> _counterAddList(int handle, List<int> values) async {
    final payload = ByteWriter()
      ..u64(handle)
      ..writeListI32(values);
    return ByteReader(await invokeAsyncMethod(MethodId.counterAddList.value, payload.takeBytes())).i32();
  }

  Future<int> _counterSetValue(int handle, int? value) async {
    final payload = ByteWriter()
      ..u64(handle)
      ..writeOptI32(value);
    return ByteReader(await invokeAsyncMethod(MethodId.counterSetValue.value, payload.takeBytes())).i32();
  }

  Future<int> _counterDuplicate(int handle) async {
    final payload = ByteWriter()..u64(handle);
    return ByteReader(await invokeAsyncMethod(MethodId.counterDuplicate.value, payload.takeBytes())).u64();
  }

  Future<int> _counterGetValue(int handle) async {
    final payload = ByteWriter()..u64(handle);
    return ByteReader(await invokeAsyncMethod(MethodId.counterGetValue.value, payload.takeBytes())).i32();
  }

  int _counterValueSync(int handle) {
    final payload = ByteWriter()..u64(handle);
    return ByteReader(invokeSyncMethod(MethodId.counterValueSync.value, payload.takeBytes())).i32();
  }

  int _counterStaticSum(int a, int b) {
    final payload = ByteWriter()
      ..i32(a)
      ..i32(b);
    return ByteReader(invokeSyncMethod(MethodId.counterStaticSum.value, payload.takeBytes())).i32();
  }

  Future<String> _counterCallDartFn(
    int handle,
    FutureOr<String> Function(String value) callback,
  ) {
    return _invokeStringToStringDartFn(MethodId.counterCallDartFn, handle, callback);
  }

  Future<int> _counterSleepAndGet(int handle, int sleepMs) async {
    final payload = ByteWriter()
      ..u64(handle)
      ..i32(sleepMs);
    return ByteReader(await invokeAsyncMethod(MethodId.counterSleepAndGet.value, payload.takeBytes())).i32();
  }

  Stream<int> _counterIncrementStream(int handle, int count, int intervalMs) {
    final id = _allocId();
    final controller = StreamController<int>(
      onCancel: () {
        _b.streamClose(_sessionId, id);
        _streams.remove(id);
      },
    );
    _streams[id] = controller;
    final payload = ByteWriter()
      ..u64(handle)
      ..i32(count)
      ..i32(intervalMs);
    _invokeAsyncRaw(makeFrame(
      type: MsgType.request,
      requestId: id,
      methodId: MethodId.counterIncrementStream.value,
      payload: payload.takeBytes(),
    ));
    return controller.stream;
  }

  /// Test helper: C++ always fails this async call with [message].
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

  /// Test helper: stream emits one value then errors with [message].
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

  /// Test helper: invoke an async-only method via the sync FFI entry and expect
  /// an error response frame.
  void invokeSyncNonSyncMethodForTest() {
    invokeSyncMethod(MethodId.add.value);
  }

  /// Test helper: call an unknown method id and expect an error Future.
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

  /// Test helper: send a truncated frame and expect an error Future.
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
    return _invokeStringToStringDartFn(MethodId.callDartHello, null, dartCallback);
  }

  /// FRB-style reverse call (C++ **sync** block on current native thread).
  ///
  /// Library does **not** move this off the io thread. If C++ calls sync on io,
  /// the scheduler stalls until Dart replies — caller's responsibility.
  Future<String> callDartHelloSync(FutureOr<String> Function(String name) dartCallback) {
    return _invokeStringToStringDartFn(MethodId.callDartHelloSync, null, dartCallback);
  }

  /// Internal helper for string-to-string DartFn reverse calls.
  ///
  /// If [handle] is non-null it is written before the fn_id, used by opaque
  /// object methods like `Counter.callCallback`. For top-level DartFn methods
  /// [handle] should be null.
  Future<String> _invokeStringToStringDartFn(
    MethodId method,
    int? handle,
    FutureOr<String> Function(String) dartCallback,
  ) async {
    final fnId = _registerDartFn(dartCallback);
    final id = _allocId();
    final c = Completer<Uint8List>();
    _pending[id] = c;
    final payload = ByteWriter();
    if (handle != null) payload.u64(handle);
    payload.u64(fnId);
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

/// Base class for opaque C++ objects exported to Dart.
///
/// Unified `dispose()` + `NativeFinalizer` attach/detach logic so generated
/// opaque classes don't have to repeat it. For the hand-written Counter fixture
/// we use `extends`; generated code may switch to `implements` if they need to
/// extend another Dart class.
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

/// Demo opaque object for hand-written class-method export test (Counter).
final class Counter extends CppOpaqueInterface {
  Counter._({required super.bridge, required super.handle});

  /// The native handle. Exposed for testing cross-isolate isolation.
  int get handle => _handle;

  /// Create a Counter with [initialValue] via the bridge constructor.
  static Future<Counter> create({int initialValue = 0}) {
    return DartCppBridge.instance.createCounter(initialValue: initialValue);
  }

  /// Default constructor: create a Counter with initial value 0.
  static Future<Counter> defaultCtor() {
    return DartCppBridge.instance.createCounterDefault();
  }

  /// Factory constructor (static method style): create a Counter with value 0.
  static Future<Counter> zero() {
    return DartCppBridge.instance.createCounterZero();
  }

  /// Increment the counter by [delta]. Defaults to 1.
  Future<void> increment([int delta = 1]) async {
    _ensureAlive();
    await _bridge._counterIncrement(_handle, delta);
  }

  /// Add all values in [values] to the counter and return the new value.
  Future<int> addList(List<int> values) async {
    _ensureAlive();
    return _bridge._counterAddList(_handle, values);
  }

  /// Set the counter to [value] if non-null. Returns the current value.
  Future<int> setValue(int? value) async {
    _ensureAlive();
    return _bridge._counterSetValue(_handle, value);
  }

  /// Create a new Counter with the same current value.
  Future<Counter> duplicate() async {
    _ensureAlive();
    final newHandle = await _bridge._counterDuplicate(_handle);
    return Counter._(bridge: _bridge, handle: newHandle);
  }

  /// Return the current value (async).
  Future<int> value() async {
    _ensureAlive();
    return _bridge._counterGetValue(_handle);
  }

  /// Return the current value synchronously via the sync FFI entry.
  int valueSync() {
    _ensureAlive();
    return _bridge._counterValueSync(_handle);
  }

  /// Static method: sum two integers on the C++ side.
  static int sum(int a, int b) {
    return DartCppBridge.instance._counterStaticSum(a, b);
  }

  /// Call a Dart callback with the current value (as a string) and return the
  /// result from Dart.
  Future<String> callCallback(FutureOr<String> Function(String value) callback) {
    _ensureAlive();
    return _bridge._counterCallDartFn(_handle, callback);
  }

  /// Normal member method: sleep on the C++ thread pool, then return the current value.
  Future<int> sleepAndGet(int sleepMs) async {
    _ensureAlive();
    return _bridge._counterSleepAndGet(_handle, sleepMs);
  }

  /// Stream member method: increment [count] times with [intervalMs] delay and
  /// emit each new value.
  Stream<int> incrementStream({int count = 5, int intervalMs = 20}) {
    _ensureAlive();
    return _bridge._counterIncrementStream(_handle, count, intervalMs);
  }
}

/// Demo struct for hand-written codegen test (Person).
class Person {
  final String name;
  final int age;

  const Person({required this.name, required this.age});

  @override
  int get hashCode => name.hashCode ^ age.hashCode;

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is Person &&
          runtimeType == other.runtimeType &&
          name == other.name &&
          age == other.age;
}

/// Demo enum for hand-written codegen test (StatusCode).
///
/// Values must match the C++ `StatusCode` enum class ordering.
enum StatusCode {
  ok,
  notFound,
  serverError,
}
