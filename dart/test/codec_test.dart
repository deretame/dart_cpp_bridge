import 'dart:typed_data';

import 'package:dart_cpp_bridge/src/codec.dart';
import 'package:test/test.dart';

void main() {
  group('codec roundtrip', () {
    test('empty payload frame', () {
      final raw = makeFrame(
        type: MsgType.request,
        requestId: 42,
        methodId: MethodId.add.value,
      );
      final f = parseFrame(raw);
      expect(f.type, MsgType.request);
      expect(f.requestId, 42);
      expect(f.methodId, MethodId.add.value);
      expect(f.payload, isEmpty);
    });

    test('i32 payload', () {
      final payload = ByteWriter()
        ..i32(-7)
        ..i32(9);
      final raw = makeFrame(
        type: MsgType.responseOk,
        requestId: 1,
        methodId: 2,
        payload: payload.takeBytes(),
      );
      final f = parseFrame(raw);
      final r = ByteReader(f.payload);
      expect(r.i32(), -7);
      expect(r.i32(), 9);
    });

    test('utf8 string including non-ascii', () {
      const s = '你好 bridge ✓';
      final payload = ByteWriter()..str(s);
      final raw = makeFrame(
        type: MsgType.responseOk,
        requestId: 3,
        methodId: MethodId.echo.value,
        payload: payload.takeBytes(),
      );
      final f = parseFrame(raw);
      expect(ByteReader(f.payload).str(), s);
    });

    test('all MsgType values encode/decode', () {
      for (final t in MsgType.values) {
        final f = parseFrame(
          makeFrame(type: t, requestId: 0, methodId: 0),
        );
        expect(f.type, t);
      }
    });

    test('dartFnCall msg type', () {
      final f = parseFrame(
        makeFrame(type: MsgType.dartFnCall, requestId: 9, methodId: 0),
      );
      expect(f.type, MsgType.dartFnCall);
      expect(f.requestId, 9);
    });

    test('pair (int, String) roundtrip', () {
      const input = (42, 'hello');
      final payload = ByteWriter()..writePairIntString(input);
      final r = ByteReader(payload.takeBytes());
      expect(r.readPairIntString(), input);
    });

    test('tuple (int, String, bool) roundtrip', () {
      const input = (1, 'a', true);
      final payload = ByteWriter()..writeTupleIntStringBool(input);
      final r = ByteReader(payload.takeBytes());
      expect(r.readTupleIntStringBool(), input);
    });
  });

  group('codec errors', () {
    test('bad magic', () {
      final raw = makeFrame(
        type: MsgType.request,
        requestId: 1,
        methodId: 1,
      );
      raw[0] = 0;
      expect(() => parseFrame(raw), throwsA(isA<StateError>()));
    });

    test('truncated frame', () {
      expect(
        () => parseFrame(Uint8List.fromList([1, 2, 3])),
        throwsA(isA<StateError>()),
      );
    });

    test('payload length longer than buffer', () {
      final raw = makeFrame(
        type: MsgType.request,
        requestId: 1,
        methodId: 1,
        payload: Uint8List.fromList([1, 2, 3, 4]),
      );
      // Corrupt payload_len to claim more bytes than available.
      // layout: magic4 ver2 type1 flags1 id8 method4 plen4 payload...
      final plenOffset = 4 + 2 + 1 + 1 + 8 + 4;
      final bd = ByteData.sublistView(raw);
      bd.setUint32(plenOffset, 9999, Endian.little);
      expect(() => parseFrame(raw), throwsA(isA<StateError>()));
    });

    test('unknown MsgType', () {
      final raw = makeFrame(
        type: MsgType.request,
        requestId: 1,
        methodId: 1,
      );
      raw[6] = 0xff; // msg_type
      expect(() => parseFrame(raw), throwsA(isA<StateError>()));
    });

    test('ByteReader truncated string', () {
      final w = ByteWriter()..u32(100);
      final r = ByteReader(w.takeBytes());
      expect(() => r.str(), throwsA(isA<StateError>()));
    });
  });
}
