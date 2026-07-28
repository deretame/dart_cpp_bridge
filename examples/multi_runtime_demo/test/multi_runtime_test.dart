import 'package:dcb_multi_runtime_demo/multi_runtime_demo.dart';
import 'package:test/test.dart';

import 'support/library_path.dart';

void main() {
  setUpAll(() async {
    await DcbLib.init(libraryPath: resolveLibraryPath());
  });

  tearDownAll(() {
    DcbLib.shutdown();
  });

  group('worker lifecycle', () {
    test('start workers', () async {
      final result = await startWorkers();
      expect(result, 'workers started');
    });

    test('stop workers', () async {
      final result = await stopWorkers();
      expect(result, 'workers stopped');
    });

    test('restart workers', () async {
      final r1 = await startWorkers();
      expect(r1, 'workers started');
      final r2 = await stopWorkers();
      expect(r2, 'workers stopped');
    });
  });

  group('cross-runtime communication', () {
    setUpAll(() async {
      await startWorkers();
    });

    tearDownAll(() async {
      await stopWorkers();
    });

    test('process message via Worker A (oneshot channel)', () async {
      final result = await processMessage(message: 'hello');
      expect(result, '[A:hello]');
    });

    test('process multiple messages sequentially', () async {
      final r1 = await processMessage(message: 'foo');
      final r2 = await processMessage(message: 'bar');
      expect(r1, '[A:foo]');
      expect(r2, '[A:bar]');
    });

    test('ping Worker B (oneshot request/reply)', () async {
      final result = await pingWorker(payload: 'test');
      expect(result, matches(RegExp(r'^\[B#\d+:test\]$')));
    });

    test('ping counter increments', () async {
      final r1 = await pingWorker(payload: 'a');
      final r2 = await pingWorker(payload: 'b');
      // Extract counter values
      final id1 = int.parse(RegExp(r'#(\d+)').firstMatch(r1)!.group(1)!);
      final id2 = int.parse(RegExp(r'#(\d+)').firstMatch(r2)!.group(1)!);
      expect(id2, greaterThan(id1));
    });

    test('pipeline: Worker A → Worker B', () async {
      final result = await pipeline(message: 'data');
      // Worker A wraps: A{data}, Worker B wraps: B[A{data}]
      expect(result, 'B[A{data}]');
    });

    test('fan-out: both workers reply', () async {
      final (a, b) = await fanOut(message: 'msg');
      expect(a, 'A:msg');
      expect(b, 'B:msg');
    });

    test('concurrent requests to different workers', () async {
      // Fire multiple requests concurrently — all should resolve.
      final futures = [
        processMessage(message: 'x1'),
        pingWorker(payload: 'x2'),
        processMessage(message: 'x3'),
        pingWorker(payload: 'x4'),
      ];
      final results = await Future.wait(futures);
      expect(results[0], '[A:x1]');
      expect(results[1], matches(RegExp(r'^\[B#\d+:x2\]$')));
      expect(results[2], '[A:x3]');
      expect(results[3], matches(RegExp(r'^\[B#\d+:x4\]$')));
    });
  });

  group('worker stream (mpsc channel)', () {
    setUpAll(() async {
      await startWorkers();
    });

    tearDownAll(() async {
      await stopWorkers();
    });

    test('receives all items from Worker A stream', () async {
      final items =
          await workerStream(count: 5, intervalMs: 10).toList();
      expect(items, hasLength(5));
      expect(items[0], 'item_0');
      expect(items[4], 'item_4');
    });

    test('stream with zero items completes immediately', () async {
      final items =
          await workerStream(count: 0, intervalMs: 0).toList();
      expect(items, isEmpty);
    });

    test('multiple streams can run sequentially', () async {
      final items1 =
          await workerStream(count: 3, intervalMs: 5).toList();
      final items2 =
          await workerStream(count: 2, intervalMs: 5).toList();
      expect(items1, hasLength(3));
      expect(items2, hasLength(2));
    });
  });

  group('DartFn from worker runtime', () {
    setUpAll(() async {
      await startWorkers();
    });

    tearDownAll(() async {
      await stopWorkers();
    });

    test('call Dart callback from Worker A (independent AsioExecutor)', () async {
      final result = await callDartFromWorkerA(
        callback: (s) async => 'dart-echo:$s',
        input: 'hello',
      );
      expect(result, 'dart-echo:hello');
    });

    test('call Dart callback from Worker B (independent AsioExecutor)', () async {
      final result = await callDartFromWorkerB(
        callback: (s) async => 'B:$s',
        input: 'world',
      );
      expect(result, 'B:world');
    });

    test('multiple sequential DartFn calls from Worker A', () async {
      final r1 = await callDartFromWorkerA(
        callback: (s) async => s.toUpperCase(),
        input: 'abc',
      );
      final r2 = await callDartFromWorkerA(
        callback: (s) async => s.toUpperCase(),
        input: 'def',
      );
      expect(r1, 'ABC');
      expect(r2, 'DEF');
    });

    test('concurrent DartFn calls from both workers', () async {
      final (a, b) = await (
        callDartFromWorkerA(
          callback: (s) async => 'fromA:$s',
          input: 'x',
        ),
        callDartFromWorkerB(
          callback: (s) async => 'fromB:$s',
          input: 'y',
        ),
      ).wait;
      expect(a, 'fromA:x');
      expect(b, 'fromB:y');
    });

    test('Dart callback with async work (delay)', () async {
      final result = await callDartFromWorkerA(
        callback: (s) async {
          await Future.delayed(Duration(milliseconds: 50));
          return 'delayed:$s';
        },
        input: 'wait',
      );
      expect(result, 'delayed:wait');
    });

    test('Dart callback that throws returns error', () async {
      final result = await callDartFromWorkerA(
        callback: (s) async => throw StateError('boom-$s'),
        input: 'err',
      );
      // Worker catches the exception and returns ERROR: prefix
      expect(result, startsWith('ERROR:'));
    });
  });
}
