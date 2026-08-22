---
title: Multiple Runtimes
description: Run independent Asio event loops with stdexec schedulers and communicate through bridge channels
---

:::note[v2.1.0]
This guide follows the released stdexec runtime. The old async-simple example
is retained only under the v1 documentation archive.
:::

`dcb::Runtime` is a process-wide singleton. When an application needs another
event loop, create a small owner around its own `io_context`, an
`dcb::IoContextScheduler`, and a thread. The worker scheduler is a normal
stdexec scheduler; it does not register a second bridge runtime or use the
removed `AsioExecutor` API.

## Worker shape

The working implementation is in
`examples/multi_runtime_demo/worker_runtime.hpp`. Its essential ownership
rules are:

- keep the `io_context`, work guard, scheduler, and thread alive together;
- start worker tasks with `stdexec::starts_on(worker.scheduler(), task)`;
- join workers only from a non-IO context when stopping, because a worker task
  may be waiting for a Dart reply delivered by the main bridge IO thread.

```cpp
class WorkerRuntime {
  DCB_ASIO_NS::io_context io_;
  std::shared_ptr<dcb::IoContextScheduler> scheduler_;
  std::unique_ptr<std::thread> thread_;

 public:
  dcb::IoContextScheduler& scheduler() { return *scheduler_; }

  template <class S>
  void spawn(S&& task) {
    exec::start_detached(
        stdexec::starts_on(*scheduler_, std::forward<S>(task))
        | stdexec::upon_error(log_error));
  }
};
```

## Cross-runtime channels

Use `co::oneshot` for one request/reply and `co::mpsc` for a stream of values.
State is passed into named coroutine functions, so the coroutine frame owns it:

```cpp
using StringTx = co::oneshot::Sender<std::string>;

stdexec::task<void> process_on_worker(StringTx tx, std::string input) {
  tx.send("processed: " + input);
  co_return;
}

stdexec::task<std::string> process(WorkerRuntime& worker,
                                   std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  worker.spawn(process_on_worker(std::move(tx), std::move(input)));
  auto reply = co_await std::move(rx);
  if (!reply) throw std::runtime_error("worker dropped");
  co_return *reply;
}
```

For a producer, await the worker scheduler's timer instead of sleeping the
worker thread:

```cpp
stdexec::task<void> produce(dcb::IoContextScheduler scheduler,
                            co::mpsc::Sender<std::string> tx) {
  for (int i = 0; i != 5; ++i) {
    co_await scheduler.schedule_after(std::chrono::milliseconds(100));
    if (!tx.send("item_" + std::to_string(i))) co_return;
  }
}
```

## Calling Dart functions from a worker

`DartFn` is awaitable from a worker task. The reply resumes the task on its
home scheduler, so the callback continuation returns to the worker loop:

```cpp
stdexec::task<void> call_dart_on_worker(
    StringTx tx, dcb::DartFn<std::string(std::string)> callback,
    std::string input) {
  try {
    tx.send(co_await callback(std::move(input)));
  } catch (const std::exception& e) {
    tx.send(std::string("ERROR: ") + e.what());
  }
}
```

Do not hold the worker mutex across `co_await`, do not block a worker loop, and
do not call `dcb::sync_wait` from an IO thread. The complete codegen fixture
also covers pipeline, fan-out, streams, worker shutdown, and DartFn errors:
`examples/multi_runtime_demo/`.

## Further reading

- [Built-in Runtime](/dart_cpp_bridge/guides/fundamentals/runtime/)
- [Foreign Runtime Integration](/dart_cpp_bridge/guides/advanced/foreign-runtime/)
- [v1 archived multi-runtime guide](/dart_cpp_bridge/v1/guides/advanced/multi-runtime/)
