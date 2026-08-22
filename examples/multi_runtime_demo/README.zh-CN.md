# stdexec 多运行时示例

这个 fixture 演示两套独立的 Asio 事件循环如何
通过 `dcb::IoContextScheduler` 暴露为 stdexec scheduler，并通过
`co::oneshot` / `co::mpsc` 与主 bridge runtime 通信。

实现使用当前 stdexec 模型：

- 导出的异步 API 返回 `stdexec::task<T>`；
- worker task 使用具名协程函数，状态通过参数进入 coroutine frame；
- `WorkerRuntime::spawn()` 使用 `stdexec::starts_on` 启动任务并记录 detached 错误；
- worker 延迟使用 `IoContextScheduler::schedule_after`，不在事件循环线程调用
  `std::this_thread::sleep_for`；
- 停止 worker 时在 blocking pool 上 join，保证主 bridge IO 线程仍能投递 DartFn 回复。

## 文件

- `worker_runtime.hpp` — 独立 worker 所有者和 scheduler；
- `native/api/multi_runtime_api.h` — `BRIDGE_ASYNC` / `BRIDGE_NORMAL` API；
- `native/api_impl/multi_runtime_api.cpp` — oneshot、mpsc、pipeline、fan-out、
  stream 和 DartFn 业务逻辑；
- `native/generated/` — 生成的 wire dispatch 和 Dart 绑定；
- `test/multi_runtime_test.dart` — 生命周期、channel、stream、并发和反向回调测试。

## 生成与构建

从 `dcb_gen_tool/` 目录通过 Dart CLI 重新生成：

```powershell
puro dart run bin/dcb_gen_tool.dart generate ../examples/multi_runtime_demo/dart_cpp_bridge.yaml
```

用 CMake 从 fixture 的 `native/` 目录构建原生库，然后运行 Dart 测试：

```powershell
cd ../examples/multi_runtime_demo
puro dart pub get
puro dart test
```

旧版 AsioExecutor/async-simple 说明保留在归档文档树中，不是本 fixture 的实现依据。
