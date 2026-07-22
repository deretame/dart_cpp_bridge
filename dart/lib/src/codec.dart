import 'dart:convert';
import 'dart:typed_data';

const int kMagic = 0x31424344; // 'DCB1' LE
const int kProtocolVersion = 1;

enum MsgType {
  request(1),
  responseOk(2),
  responseErr(3),
  streamData(4),
  streamEnd(5),
  streamErr(6),
  dartFnCall(7);

  final int value;
  const MsgType(this.value);

  static MsgType from(int v) {
    for (final e in MsgType.values) {
      if (e.value == v) return e;
    }
    throw StateError('unknown MsgType $v');
  }
}

enum MethodId {
  bridgeVersion(1),
  add(2),
  sleepTest(3),
  ticks(4),
  echo(5),
  failAsync(6),
  failStream(7),
  callDartHello(8);

  final int value;
  const MethodId(this.value);
}

class ByteWriter {
  final BytesBuilder _b = BytesBuilder(copy: false);

  void u8(int v) => _b.addByte(v & 0xff);

  void u16(int v) {
    _b.addByte(v & 0xff);
    _b.addByte((v >> 8) & 0xff);
  }

  void u32(int v) {
    final bd = ByteData(4)..setUint32(0, v, Endian.little);
    _b.add(bd.buffer.asUint8List());
  }

  void u64(int v) {
    final bd = ByteData(8)..setUint64(0, v, Endian.little);
    _b.add(bd.buffer.asUint8List());
  }

  // expose for composing payloads
  void writeRaw(List<int> data) => _b.add(data);

  void i32(int v) {
    final bd = ByteData(4)..setInt32(0, v, Endian.little);
    _b.add(bd.buffer.asUint8List());
  }

  void str(String s) {
    final bytes = utf8.encode(s);
    u32(bytes.length);
    _b.add(bytes);
  }

  void bytes(List<int> data) => _b.add(data);

  Uint8List takeBytes() => _b.takeBytes();
}

class ByteReader {
  ByteReader(this.data) : _bd = ByteData.sublistView(data);

  final Uint8List data;
  final ByteData _bd;
  int _pos = 0;

  void _need(int n) {
    if (_pos + n > data.length) {
      throw StateError('codec: truncated');
    }
  }

  int u8() {
    _need(1);
    return data[_pos++];
  }

  int u16() {
    _need(2);
    final v = _bd.getUint16(_pos, Endian.little);
    _pos += 2;
    return v;
  }

  int u32() {
    _need(4);
    final v = _bd.getUint32(_pos, Endian.little);
    _pos += 4;
    return v;
  }

  int u64() {
    _need(8);
    final v = _bd.getUint64(_pos, Endian.little);
    _pos += 8;
    return v;
  }

  int i32() {
    _need(4);
    final v = _bd.getInt32(_pos, Endian.little);
    _pos += 4;
    return v;
  }

  String str() {
    final n = u32();
    _need(n);
    final s = utf8.decode(data.sublist(_pos, _pos + n));
    _pos += n;
    return s;
  }

  Uint8List takeRest() => data.sublist(_pos);
}

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

class Frame {
  Frame({
    required this.type,
    required this.flags,
    required this.requestId,
    required this.methodId,
    required this.payload,
  });

  final MsgType type;
  final int flags;
  final int requestId;
  final int methodId;
  final Uint8List payload;
}

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
