import 'package:dcb_foreign_runtime_demo/foreign_runtime_demo.dart';
import 'package:test/test.dart';

import 'support/library_path.dart';

void main() {
  setUpAll(() async {
    await DcbLib.init(libraryPath: libraryPath);
  });

  tearDownAll(() {
    DcbLib.shutdown();
  });

  group('libuv foreign runtime', () {
    test('start and stop uv worker', () async {
      final startResult = await startUvWorker();
      expect(startResult, 'uv worker started');

      final stopResult = await stopUvWorker();
      expect(stopResult, 'uv worker stopped');
    });

    test('ask_uv: oneshot request/reply via libuv', () async {
      await startUvWorker();

      final result = await askUv(message: 'hello');
      expect(result, '[uv:hello]');

      final result2 = await askUv(message: 'world');
      expect(result2, '[uv:world]');

      await stopUvWorker();
    });

    test('uv_compute: CPU task on libuv thread', () async {
      await startUvWorker();

      final sum = await uvCompute(n: 100);
      expect(sum, 5050); // 1+2+...+100

      final sum2 = await uvCompute(n: 10);
      expect(sum2, 55);

      await stopUvWorker();
    });

    test('uv_stream: stream from libuv worker', () async {
      await startUvWorker();

      final items = await uvStream(count: 3, intervalMs: 10).toList();
      expect(items, ['uv_item_0', 'uv_item_1', 'uv_item_2']);

      await stopUvWorker();
    });

    test('multiple concurrent requests', () async {
      await startUvWorker();

      // 并发发送多个请求
      final results = await Future.wait([
        askUv(message: 'a'),
        askUv(message: 'b'),
        askUv(message: 'c'),
      ]);

      expect(results, ['[uv:a]', '[uv:b]', '[uv:c]']);

      await stopUvWorker();
    });

    test('error when worker not running', () async {
      // 确保 worker 已停止
      await stopUvWorker();

      expect(
        () => askUv(message: 'test'),
        throwsA(isA<Object>()),
      );
    });

    test('restart worker after stop', () async {
      await startUvWorker();
      final r1 = await askUv(message: 'first');
      expect(r1, '[uv:first]');

      await stopUvWorker();

      // 重新启动
      await startUvWorker();
      final r2 = await askUv(message: 'second');
      expect(r2, '[uv:second]');

      await stopUvWorker();
    });
  });

  group('DartFn from libuv foreign runtime', () {
    setUpAll(() async {
      await startUvWorker();
    });

    tearDownAll(() async {
      await stopUvWorker();
    });

    test('call Dart callback from libuv loop (ForeignExecutor)', () async {
      final result = await callDartFromUv(
        callback: (s) async => 'uv-echo:$s',
        input: 'hello',
      );
      expect(result, 'uv-echo:hello');
    });

    test('multiple sequential calls from libuv', () async {
      final r1 = await callDartFromUv(
        callback: (s) async => s.toUpperCase(),
        input: 'abc',
      );
      final r2 = await callDartFromUv(
        callback: (s) async => s.toUpperCase(),
        input: 'xyz',
      );
      expect(r1, 'ABC');
      expect(r2, 'XYZ');
    });

    test('Dart callback with async delay from libuv', () async {
      final result = await callDartFromUv(
        callback: (s) async {
          await Future.delayed(Duration(milliseconds: 30));
          return 'delayed:$s';
        },
        input: 'wait',
      );
      expect(result, 'delayed:wait');
    });

    test('Dart callback that throws returns error from libuv', () async {
      final result = await callDartFromUv(
        callback: (s) async => throw StateError('uv-boom-$s'),
        input: 'err',
      );
      expect(result, startsWith('ERROR:'));
    });
  });

  group('cbridge pure C API', () {
    test('dcb_async_create + dcb_async_complete + async_wait', () async {
      final result = await testCbridgeAsync();
      expect(result, 'cbridge_ok');
    });

    test('dcb_async_fail propagates error to coroutine', () async {
      final result = await testCbridgeAsyncFail();
      expect(result, 'CAUGHT:intentional_error');
    });

    test('dcb_async_cancel propagates cancellation to coroutine', () async {
      final result = await testCbridgeAsyncCancel();
      expect(result, 'CAUGHT:async_wait: operation cancelled');
    });

    test('channel service: mpsc long-lived service on uv worker', () async {
      await startUvWorker();

      final result = await testChannelService();
      expect(result, '[svc:msg0],[svc:msg1],[svc:msg2]');

      await stopUvWorker();
    });

    test('channel service concurrent: batch send then collect replies', () async {
      await startUvWorker();

      final result = await testChannelServiceConcurrent();
      expect(result, '[svc:c0],[svc:c1],[svc:c2],[svc:c3],[svc:c4]');

      await stopUvWorker();
    });

    test('dcb_invoke_dart_fn: C callback-style DartFn invocation', () async {
      final result = await testCbridgeInvoke(
        callback: (s) async => 'c-echo:$s',
        input: 'hello',
      );
      expect(result, 'c-echo:hello');
    });

    test('dcb_invoke_dart_fn: Dart callback with delay', () async {
      final result = await testCbridgeInvoke(
        callback: (s) async {
          await Future.delayed(Duration(milliseconds: 30));
          return 'delayed:$s';
        },
        input: 'wait',
      );
      expect(result, 'delayed:wait');
    });

    test('dcb_invoke_dart_fn: Dart callback that throws', () async {
      final result = await testCbridgeInvoke(
        callback: (s) async => throw StateError('boom-$s'),
        input: 'err',
      );
      expect(result, startsWith('ERROR:'));
    });
  });
}
