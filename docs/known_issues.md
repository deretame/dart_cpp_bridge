# 已知问题与技术债

> 记录实现过程中已确认的卡点，避免重复踩坑。  
> 更新日期：2026-07-22

---

## 1. 【已解决】DartFn / 外部事件可在 io 上真 `co_await`

### 1.1 目标形态（已达成）

```text
io 协程:  post DartFnCall → co_await oneshot（挂起，io 线程去干别的）
Dart Isolate: 执行回调 → dcb_dart_fn_reply → oneshot.send
io:       Executor::schedule(resume) → 继续业务
```

- **不会**在 io 上 `future.get()` / 忙等；
- 占用的是「协程挂起」，不是 pool 工作线程。

### 1.2 做法（对齐 FRB oneshot）

| 组件 | 角色 |
|------|------|
| `include/dart_cpp_bridge/channel.hpp` | `co::oneshot`：`coAwait(Executor*)` + `wake_waiter` → `ex->schedule(resume)` |
| `include/dart_cpp_bridge/asio_executor.hpp` | `AsioExecutor`：`schedule` = `asio::post(io, …)` |
| `Runtime::spawn_on_asio` | `lazy.via(executor).start(...)`，保证 Lazy 绑 executor |
| `Session::invoke_dart_fn_async` | `call_id → oneshot tx`；`co_await rx.recv()` |
| `kCallDartHello` | io 上 `co_await cb.callAsync(...)` |

`callSync` 仍为当前线程 `std::promise` + `get()`；堵 io 自负。

### 1.3 历史踩坑（保留备查）

| 尝试 | 现象 | 判断 |
|------|------|------|
| `async_simple::Promise` + FutureAwaiter | 30s 超时 | 与 executor 完成约定不匹配 |
| 仅 `asio::post(io, setValue)` | 仍不稳 | 未走 Lazy 的 `coAwait(Executor*)` |
| 裸 `coroutine_handle::resume` | AV | 不能当标准 coro 乱 resume |
| pool + `get()` | 能通但不干净 | 已替换为 oneshot |
| `spawn_on_asio` 里 coroutine lambda capture | gen=0 / AV | factory 在 `start()` 后销毁，capture 悬空；须 `shared_ptr` 保活到 Lazy 结束 |
| 成员函数 coroutine 读 `this->field` | 偶发错值 | `callAsync` 改为静态 Lazy，参数 by-value |

### 1.4 相关代码

- `include/dart_cpp_bridge/channel.hpp`
- `include/dart_cpp_bridge/asio_executor.hpp`
- `include/dart_cpp_bridge/dart_fn.hpp` — `callSync` / `callAsync`
- `src/runtime/runtime.cpp` — `invoke_dart_fn_async` / `complete_dart_fn`
- `src/wire/demo_api.cpp` — `kCallDartHello` / `kCallDartHelloSync`
- `examples/phase1_demo/smoke_main.cpp` — oneshot 跨线程唤醒 + io 不堵 测试

---

## 2. 【已绕过】normal / sleepTest 不宜依赖 `spawn_blocking`+FutureAwaiter

- **现象**：早期 wire 用 `co_await spawn_blocking(sleep)` 会挂起不返回。  
- **现状**：`sleepTest` / `ticks` 间隔等改为 `asio::post(thread_pool, ...)`，结果再 `post` 回业务（或直接 `try_post`）。  
- **后续**：若实现 `spawn_blocking` Lazy，应复用 oneshot / 同一套 `Executor::schedule` 唤醒。

---

## 3. 【已接受】设计演进：Session 每 Isolate 一个

- 设计原文偏单 session。  
- 为实现后台 Isolate 的 async/stream，改为 **Runtime 进程唯一 + Session 每 Isolate**。  
- 见 [progress.md](./progress.md) §3。

---

## 4. 【原则】不为用户兜底阻塞 io

- Sync DartFn：**不**自动离载到 pool。  
- 用户在 io 上 `callSync` → 调度器停转 → **用户问题**。  
- 文档与 API 命名需持续强调，避免误用。

---

## 5. 一句话

**DartFn 反向调用：协议 + oneshot + AsioExecutor 已通；async 路径为 io 上真挂起，sync 路径仍阻塞当前线程。**
