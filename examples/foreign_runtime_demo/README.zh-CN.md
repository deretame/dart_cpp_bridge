# Foreign Runtime Demo — 把 libuv 封装为 stdexec scheduler

演示非 asio 事件循环（libuv）如何以 **stdexec scheduler** 的身份接入 bridge 的
sender 世界。这是旧 `ForeignExecutor` + `foreign_runtime.h` C 注册 API
（两者均已删除）在 stdexec 迁移后的替代方案。

## 架构

```
Dart Isolate ⇄ bridge io_context (dcb_runtime)
                  ⇅ starts_on / schedule_after / uv_work
UvWorker: uv_loop_t + loop 线程  (UvScheduler, 见 uv_scheduler.hpp)
```

- `UvWorker` 持有 `uv_loop_t` + 专用线程,对外暴露 **`UvScheduler`**
  （`uv_scheduler.hpp`）,一个标准的 stdexec scheduler:
  - `schedule()` — 在 loop 线程上执行一次任务（mutex + `uv_async_send` 唤醒）
  - `schedule_after(d)` — `uv_timer_t` 定时器 sender,支持 stop token 取消
  - `uv_work(f)` — `uv_queue_work`（libuv 线程池）,完成回调在 loop 线程交付
- 业务函数是 `exec::task` 协程,用 `stdexec::starts_on(worker.scheduler(), ...)`
  组合;exec::task 自动把完成 reschedule 回调用方的 home scheduler（io 线程）。
- wire dispatch（`native/generated/wire_dispatch.cpp`）在 io 线程上以
  `exec::task` 协程启动异步方法（`starts_on` + `exec::start_detached`）。
  使用静态协程函数而非协程 lambda（MSVC 19.51 捕获 bug,见 cbridge.cpp 注释）。

## 目录

```
├── uv_scheduler.hpp      # UvScheduler: schedule / schedule_after / uv_work
├── uv_worker.hpp         # UvWorker: uv_loop_t + 线程,提供 scheduler()
├── native/
│   ├── CMakeLists.txt    # 构建 dcb_foreign_runtime_demo（链接 libuv）
│   ├── api/foreign_api.h # BRIDGE_* API（exec::task 签名）
│   ├── api_impl/foreign_api.cpp  # 业务逻辑（sender 组合）
│   └── generated/        # wire_dispatch.*（手改为 std::exec 模式）
├── lib/                  # 生成的 Dart API
└── test/foreign_runtime_test.dart  # 19 个测试
```

## 构建与测试

```bash
# 1. 配置 + 构建原生库（libuv 由 CMake FetchContent 拉取）
cd examples/foreign_runtime_demo
cmake -S native -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

# 2. 运行 Dart 测试（build hooks 会通过 CMake 编译库）
dart pub get
dart test test/foreign_runtime_test.dart
```

若 `cmake` 不在 PATH,用 `NIX_DCB_CMAKE` 指定（如
`NIX_DCB_CMAKE=/path/to/cmake.exe dart test ...`）。

## 覆盖场景（19 个测试）

- uv worker 启动/停止/重启;ask_uv 请求回复;uv_compute（loop 线程上计算）;
  uv_stream（uv 定时器驱动的异步流）;并发请求
- 从 uv loop 发起的 DartFn 反向调用;Dart 侧异常转为 `ERROR:...`
- cbridge 纯 C API:`dcb_async_create/complete/fail/cancel`、`async_wait`、
  `dcb_invoke_dart_fn`（线程版与纯 C 回调版）
- channel service 模式:mpsc 请求/回复服务循环（uv 线程）、批量发送后收集回复

## 设计说明

见 `docs/foreign_runtime_design.md`。
