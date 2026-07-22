import 'dart:convert';
import 'dart:typed_data';

/// Little-endian magic `'DCB1'`.
const int kMagic = 0x31424344; // 'DCB1' LE

/// Wire protocol version currently spoken by this package.
const int kProtocolVersion = 1;

/// Frame type on the binary wire.
enum MsgType {
  /// Dart → C++ method invocation.
  request(1),

  /// Successful reply payload.
  responseOk(2),

  /// Error reply (code + message).
  responseErr(3),

  /// One stream item.
  streamData(4),

  /// Stream completed.
  streamEnd(5),

  /// Stream failed.
  streamErr(6),

  /// C++ → DartFn reverse call.
  dartFnCall(7);

  /// Numeric tag written on the wire.
  final int value;
  const MsgType(this.value);

  /// Parse a wire tag or throw [StateError].
  static MsgType from(int v) {
    for (final e in MsgType.values) {
      if (e.value == v) return e;
    }
    throw StateError('unknown MsgType $v');
  }
}

/// Demo / hand-written method ids (Phase 1; codegen will replace later).
enum MethodId {
  /// Sync: protocol version as `i32`.
  bridgeVersion(1),

  /// Async: `i32 + i32 → i32`.
  add(2),

  /// Normal (pool): returns a done string after sleep.
  sleepTest(3),

  /// Stream of tick indices.
  ticks(4),

  /// Async echo of a UTF-8 string.
  echo(5),

  /// Async that always fails (tests).
  failAsync(6),

  /// Stream that emits then errors (tests).
  failStream(7),

  /// DartFn reverse call; C++ awaits on io.
  callDartHello(8),

  /// DartFn reverse call; C++ blocks current native thread.
  callDartHelloSync(9),

  /// Async optional i32 -> optional i32 test.
  maybeDouble(10),

  /// Async list i32 -> i32 sum test.
  sumVec(11),

  /// Async Uint8List -> Uint8List reverse test.
  reverseBytes(12),

  /// Async enum -> enum next test.
  nextStatus(13),

  /// Async fixed array (4 × i32) -> i32 sum test.
  sumFixedFour(14);

  /// Numeric method id on the wire.
  final int value;
  const MethodId(this.value);
}

/// Little-endian binary writer used for frames and payloads.
class ByteWriter {
  final BytesBuilder _b = BytesBuilder(copy: false);

  /// Append one unsigned byte.
  void u8(int v) => _b.addByte(v & 0xff);

  /// Append little-endian `u16`.
  void u16(int v) {
    _b.addByte(v & 0xff);
    _b.addByte((v >> 8) & 0xff);
  }

  /// Append little-endian `u32`.
  void u32(int v) {
    final bd = ByteData(4)..setUint32(0, v, Endian.little);
    _b.add(bd.buffer.asUint8List());
  }

  /// Append little-endian `u64`.
  void u64(int v) {
    final bd = ByteData(8)..setUint64(0, v, Endian.little);
    _b.add(bd.buffer.asUint8List());
  }

  /// Append raw bytes without a length prefix.
  void writeRaw(List<int> data) => _b.add(data);

  /// Append little-endian `i32`.
  void i32(int v) {
    final bd = ByteData(4)..setInt32(0, v, Endian.little);
    _b.add(bd.buffer.asUint8List());
  }

  /// Append a nullable `i32` (1-byte presence tag + value if non-null).
  void writeOptI32(int? v) {
    if (v == null) {
      u8(0);
    } else {
      u8(1);
      i32(v);
    }
  }

  /// Append a length-prefixed list of `i32`.
  void writeListI32(List<int> v) {
    u32(v.length);
    for (final x in v) {
      i32(x);
    }
  }

  /// Append a length-prefixed raw byte buffer as `Uint8List`.
  void writeUint8List(Uint8List v) {
    u32(v.length);
    bytes(v);
  }

  /// Append an enum value as `i32` (underlying value / index).
  void writeEnum(int v) => i32(v);

  /// Append a fixed-length array of `i32` (without length prefix).
  void writeFixedArrayI32(List<int> v) {
    for (final x in v) {
      i32(x);
    }
  }

  /// Append `u32` length + UTF-8 bytes.
  void str(String s) {
    final bytes = utf8.encode(s);
    u32(bytes.length);
    _b.add(bytes);
  }

  /// Append raw bytes (alias of [writeRaw]).
  void bytes(List<int> data) => _b.add(data);

  /// Take ownership of the built buffer.
  Uint8List takeBytes() => _b.takeBytes();
}

/// Little-endian binary reader for frames and payloads.
class ByteReader {
  /// Create a reader over [data]; position starts at 0.
  ByteReader(this.data) : _bd = ByteData.sublistView(data);

  /// Underlying buffer.
  final Uint8List data;
  final ByteData _bd;
  int _pos = 0;

  void _need(int n) {
    if (_pos + n > data.length) {
      throw StateError('codec: truncated');
    }
  }

  /// Read one unsigned byte.
  int u8() {
    _need(1);
    return data[_pos++];
  }

  /// Read little-endian `u16`.
  int u16() {
    _need(2);
    final v = _bd.getUint16(_pos, Endian.little);
    _pos += 2;
    return v;
  }

  /// Read little-endian `u32`.
  int u32() {
    _need(4);
    final v = _bd.getUint32(_pos, Endian.little);
    _pos += 4;
    return v;
  }

  /// Read little-endian `u64`.
  int u64() {
    _need(8);
    final v = _bd.getUint64(_pos, Endian.little);
    _pos += 8;
    return v;
  }

  /// Read little-endian `i32`.
  int i32() {
    _need(4);
    final v = _bd.getInt32(_pos, Endian.little);
    _pos += 4;
    return v;
  }

  /// Read a nullable `i32` (1-byte presence tag + value if non-null).
  int? readOptI32() {
    final hasValue = u8() != 0;
    if (!hasValue) return null;
    return i32();
  }

  /// Read a length-prefixed list of `i32`.
  List<int> readListI32() {
    final n = u32();
    final result = <int>[];
    for (var i = 0; i < n; i++) {
      result.add(i32());
    }
    return result;
  }

  /// Read a length-prefixed raw byte buffer as `Uint8List`.
  Uint8List readUint8List() {
    final n = u32();
    _need(n);
    final result = Uint8List.fromList(data.sublist(_pos, _pos + n));
    _pos += n;
    return result;
  }

  /// Read an enum value as `i32` (underlying value / index).
  int readEnum() => i32();

  /// Read a fixed-length array of `i32` (without length prefix).
  List<int> readFixedArrayI32(int n) {
    return List.generate(n, (_) => i32());
  }

  /// Read `u32` length + UTF-8 string.
  String str() {
    final n = u32();
    _need(n);
    final s = utf8.decode(data.sublist(_pos, _pos + n));
    _pos += n;
    return s;
  }

  /// Remaining unread bytes.
  Uint8List takeRest() => data.sublist(_pos);
}

/// Encode one protocol frame.
Uint8List makeFrame({
  required MsgType type,
  required int requestId,
  required int methodId,
  Uint8List? payload,
}) {
  final p = payload ?? Uint8List(0);
  final w = ByteWriter()
    ..u32(kMagic)
    ..u16(kProtocolVersion)
    ..u8(type.value)
    ..u8(0)
    ..u64(requestId)
    ..u32(methodId)
    ..u32(p.length)
    ..bytes(p);
  return w.takeBytes();
}

/// Parsed protocol frame.
class Frame {
  /// Create a decoded frame.
  Frame({
    required this.type,
    required this.flags,
    required this.requestId,
    required this.methodId,
    required this.payload,
  });

  /// Message kind.
  final MsgType type;

  /// Reserved flags byte.
  final int flags;

  /// Multiplexing id (request / stream / DartFn reply).
  final int requestId;

  /// [MethodId] value (or 0 when not applicable).
  final int methodId;

  /// Payload bytes after the fixed header.
  final Uint8List payload;
}

/// Decode one frame or throw [StateError] on bad magic/version/truncation.
Frame parseFrame(Uint8List data) {
  final r = ByteReader(data);
  if (r.u32() != kMagic) throw StateError('bad magic');
  if (r.u16() != kProtocolVersion) throw StateError('bad version');
  final type = MsgType.from(r.u8());
  final flags = r.u8();
  final requestId = r.u64();
  final methodId = r.u32();
  final plen = r.u32();
  final rest = r.takeRest();
  if (rest.length < plen) throw StateError('payload truncated');
  return Frame(
    type: type,
    flags: flags,
    requestId: requestId,
    methodId: methodId,
    payload: rest.sublist(0, plen),
  );
}
