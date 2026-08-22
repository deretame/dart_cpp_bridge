# Multi-runtime stdexec demo

This fixture demonstrates two
independent Asio event loops, each exposed through `dcb::IoContextScheduler`,
communicating with the bridge runtime through `co::oneshot` and `co::mpsc`.

The implementation deliberately uses current stdexec APIs:

- exported async APIs return `stdexec::task<T>`;
- worker tasks are named coroutine functions with state passed as parameters;
- `WorkerRuntime::spawn()` starts tasks with `stdexec::starts_on` and reports
  detached errors;
- worker delays use `IoContextScheduler::schedule_after`, never
  `std::this_thread::sleep_for` on an event-loop thread;
- worker shutdown joins on the blocking pool so the main bridge IO thread can
  continue delivering DartFn replies.

## Files

- `worker_runtime.hpp` — independent worker owner and scheduler;
- `native/api/multi_runtime_api.h` — `BRIDGE_ASYNC`/`BRIDGE_NORMAL` API;
- `native/api_impl/multi_runtime_api.cpp` — oneshot, mpsc, pipeline, fan-out,
  stream, and DartFn business logic;
- `native/generated/` — generated wire dispatch and Dart bindings;
- `test/multi_runtime_test.dart` — lifecycle, channel, stream, concurrency,
  and reverse-callback coverage.

## Generate and build

From `dcb_gen_tool/`, regenerate the fixture through the Dart CLI:

```powershell
puro dart run bin/dcb_gen_tool.dart generate ../examples/multi_runtime_demo/dart_cpp_bridge.yaml
```

Build the native library from the fixture's `native/` directory with CMake,
then run the Dart tests:

```powershell
cd ../examples/multi_runtime_demo
puro dart pub get
puro dart test
```

The legacy AsioExecutor/async-simple explanation is kept in the archived
documentation tree; it is not the source for this fixture.
