# 外部运行时集成设计 (Foreign Runtime Integration)

> 状态：已实施（stdexec 迁移后版本）
> 目标：让非 asio 的事件循环（libuv、glib、自定义 loop 等）作为 **stdexec scheduler** 接入 bridge，
> 与主运行时进行非阻塞消息传递，并使用标准 sender 算法组合业务逻辑。

## 1. 模型

stdexec 迁移后（见 `docs/cpp26_executor_model_usage.md`），外部运行时不再需要实现
`async_simple::Executor` 接口，也不再需要 `foreign_runtime.h` 的 C 注册 API（两者均已删除）。
一个外部事件循环只要提供 **scheduler**（`schedule()` → “在该 loop 线程上执行一次”的 sender），
就能与 bridge 的 `co::oneshot` / `co::mpsc` / `DartFn` 全部互通：

```text
业务协程 (exec::task)
  → co_await stdexec::starts_on(外部scheduler, work)   // 在外部 loop 线程执行
  → 完成时 exec::task 自动 reschedule 回 home scheduler
```

### 参考实现：`examples/foreign_runtime_demo/uv_scheduler.hpp`

- **`UvScheduler`**：`uv_loop_t` 包装。`schedule()` 的 sender 通过
  `mutex + uv_async_send` 把任务投递到 loop 线程执行（start 队列 + 唤醒）。
- **`schedule_after(d)`**：`uv_timer_t` 定时器 sender，完成于 loop 线程；
  通过 atomic claim 仲裁完成/取消，支持接收者 stop token（停止 → `set_stopped`）。
- **`uv_work(f)`**：`uv_queue_work` 封装——`f` 在 libuv 线程池执行，
  值/异常在 loop 线程交付（`set_value` / `set_error`）。
- **`UvWorker`**：owns `uv_loop_t` + loop 线程，暴露 `scheduler()`。
  生命周期规则与 `IoContextScheduler` 相同（scheduler 不得越过 worker 存活；
  `stop()` 先置 closed、唤醒、join、`uv_walk` 清理残留 handle 后 `uv_loop_close`）。

### 业务代码形态

```cpp
// 在 uv loop 线程上跑一段工作，完成回 io 线程：
auto result = co_await stdexec::starts_on(
    worker.scheduler(),
    stdexec::just(msg) | stdexec::then([](std::string m) { return "[uv:" + m + "]"; }));

// 定时器（可取消）：
co_await worker.scheduler().schedule_after(50ms);
```

## 2. 与旧方案的差异

| 旧方案（已删除）                      | 新方案                                 |
| ------------------------------------- | -------------------------------------- |
| `ForeignExecutor`（async_simple::Executor 实现） | `UvScheduler`（stdexec scheduler）     |
| `foreign_runtime.h` C 注册 API（dcb_foreign_register / dcb_foreign_executor / …） | 无——直接构造 scheduler 对象            |
| `Lazy.via(executor)`                  | `stdexec::starts_on(sched, sndr)` / `exec::task` |
| `coro::sleep(dur, executor)`          | `sched.schedule_after(dur)`（stop token 取消） |
| `Signal` / `Slot` 取消                | `inplace_stop_source` / `inplace_stop_callback` |

## 3. 注意事项

- **不要在协程内持有全局锁跨 `co_await`**：io 线程需要保持空闲才能恢复协程；
  锁只用于获取 scheduler / 启动任务，挂起前释放（见 `foreign_api.cpp` 各函数）。
- **MSVC 19.51 协程 lambda 捕获 bug**：生成代码中的异步 dispatch 一律使用
  静态协程函数 + 参数传递（见 `wire_dispatch.cpp` 与 `cbridge.cpp` 注释）。
- 已删除文件：`foreign_executor.hpp`、`foreign_runtime.h`、`src/foreign_runtime.cpp`。
