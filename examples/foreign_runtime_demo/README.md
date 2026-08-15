# Foreign Runtime Demo — libuv as a stdexec Scheduler (v2)

Demonstrates how a non-asio event loop (libuv) integrates with the bridge's
stdexec sender world. This is the stdexec-era replacement for the old
`ForeignExecutor` + `foreign_runtime.h` C registration API (both deleted).

## Architecture

```
Dart Isolate ⇄ bridge io_context (dcb_runtime)
                  ⇅ starts_on / schedule_after / uv_work
UvWorker: uv_loop_t + loop thread  (UvScheduler, see uv_scheduler.hpp)
```

- `UvWorker` owns a `uv_loop_t` + a dedicated thread and exposes a
  **`UvScheduler`** (`uv_scheduler.hpp`), a plain stdexec scheduler:
  - `schedule()` — run a task once on the loop thread
    (mutex + `uv_async_send` wake-up)
  - `schedule_after(d)` — `uv_timer_t` sender, cancellable via stop tokens
  - `uv_work(f)` — `uv_queue_work` (libuv thread pool), completion on the loop
- Business functions are `stdexec::task` coroutines composed with
  `stdexec::starts_on(worker.scheduler(), ...)`; the task reschedules
  completions back to the caller's home scheduler (the io thread).
- The wire dispatch (`native/generated/wire_dispatch.cpp`) launches async
  methods as `stdexec::task` coroutines on the io thread
  (`starts_on` + `exec::start_detached`). Static coroutine functions are used
  instead of coroutine lambdas (MSVC 19.51 capture bug, see cbridge.cpp).

## Layout

```
├── uv_scheduler.hpp      # UvScheduler: schedule / schedule_after / uv_work
├── uv_worker.hpp         # UvWorker: uv_loop_t + thread, scheduler()
├── native/
│   ├── CMakeLists.txt    # builds dcb_foreign_runtime_demo (links libuv)
│   ├── api/foreign_api.h # BRIDGE_* API (stdexec::task signatures)
│   ├── api_impl/foreign_api.cpp  # business logic (sender composition)
│   └── generated/        # wire_dispatch.* (hand-ported to std::exec)
├── lib/                  # generated Dart API
└── test/foreign_runtime_test.dart  # 19 tests
```

## Build & test

```bash
# 1. Configure + build the native library (libuv is fetched via CMake)
cd examples/foreign_runtime_demo
cmake -S native -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 2. Run the Dart tests (build hooks compile the library via CMake)
dart pub get
dart test test/foreign_runtime_test.dart
```

If `cmake` is not on PATH, pass it via `NIX_DCB_CMAKE` (e.g.
`NIX_DCB_CMAKE=/path/to/cmake.exe dart test ...`).

## Covered scenarios (19 tests)

- start/stop/restart the uv worker; ask_uv request/reply; uv_compute (CPU on
  the loop thread); uv_stream (async stream via uv timer); concurrent requests
- DartFn reverse calls started from the uv loop; Dart exceptions surface as
  `ERROR:...`
- cbridge pure C API: `dcb_async_create/complete/fail/cancel`, `async_wait`,
  `dcb_invoke_dart_fn` (thread-based and pure-C callback paths)
- channel service mode: mpsc request/reply loop on the uv thread, batch send +
  collect replies

## Design notes

See `docs/foreign_runtime_design.md`.
