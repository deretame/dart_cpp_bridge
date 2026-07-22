# 已知问题与技术债

> 记录实现过程中已确认的卡点，避免重复踩坑。  
> 更新日期：2026-07-22

---

## 1. 【未解决】DartFn / 外部事件无法稳定 `co_await` 挂在 io 上

### 1.1 目标形态（尚未达成）

```text
io 协程:  post DartFnCall → co_await（挂起，io 线程去干别的）
Dart Isolate: 执行回调 → dcb_dart_fn_reply
io:       resume 协程 → 继续业务
```

要求：

- **不能**在 io 线程上 `future.get()` / 忙等（会卡住整桥单线程调度器）；
- 占用的应是「协程挂起」，而不是 pool 工作线程。

### 1.2 现状（权宜）

| 入口 | 实际行为 |
|------|----------|
| `callSync` / `callDartHelloSync` | **当前线程** `std::promise` + `future.get()` 阻塞等 |
| `callDartHello`（对外叫 async） | 丢到 **thread_pool** 上再 `get()`，**不堵 io**，但 **占 pool 线程** |

Dart 侧回调（sync/async 闭包）本身没问题：始终在 **Dart Isolate 事件循环**执行，协议 `DartFnCall` + `dcb_dart_fn_reply` 已跑通（有测试）。

**卡的是 C++ 侧：async_simple Lazy 如何被「Dart FFI 完成」合法唤醒。**

### 1.3 已尝试与结果

| 尝试 | 现象 | 判断 |
|------|------|------|
| `async_simple::Promise` + `co_await Future`（FutureAwaiter） | 常见 **30s 超时**（continuation 未恢复） | 与 executor / 完成线程约定不匹配 |
| 在 `complete` 里 `asio::post(io, setValue)` | 仍超时或不稳定 | 仅 post 到 io 不够 |
| 自定义 Awaiter + 保存 `std::coroutine_handle<>` 再 `resume` | **Access Violation**（`isolate_group=nil`，崩在 native 线程） | 与 async_simple Lazy 的句柄/调度 **不兼容**，不能当标准 coro 乱 resume |
| Lazy `.via(AsioExecutor)` 绑 asio | 一度 **连 sync 路径也挂死** | executor 接入方式未验证清楚，回滚 |
| `spawn_blocking` 内 `co_await Future` | 早期 **sleepTest 类路径挂死** | 同属 FutureAwaiter 问题；normal 已改为 pool + 直接 `post` 结果，绕开 |

### 1.4 问题本质（当前结论）

1. **协议层 OK**：port 投递、Dart 执行、FFI reply、session 表均可用。  
2. **缺的是调度层**：`async_simple` 与 `asio::io_context` 之间缺少一套已验证的  
   「外部完成 → 在正确 executor 上 resume Lazy」接法。  
3. **不能**用裸 `std::coroutine_handle::resume` 去唤醒 async_simple 的 Lazy。  
4. 库原则：**不替用户把阻塞从 io 偷偷挪到 pool**；sync 堵 io 自负。async 入口目前用 pool 阻塞，是 **实现债**，不是最终设计。

### 1.5 建议后续方向

1. 精读 async_simple：`Promise` / `Future` / `Executor::schedule` / `Lazy::via` 官方示例，做 **最小可复现**（无 Dart，仅 asio 定时器完成 Promise）。  
2. 在最小复现稳定后，把 `complete_dart_fn` 接到同一套 `Executor::schedule(setValue)`。  
3. 再把 `callAsync` 改为真正的 io 上 `co_await`，async 入口可去掉 pool 阻塞。  
4. 同步整理 `spawn_blocking`：与 DartFn 共用同一套「外部完成唤醒」机制。

### 1.6 相关代码

- `include/dart_cpp_bridge/dart_fn.hpp` — `callSync` / `callAsync`  
- `include/dart_cpp_bridge/session.hpp` — `invoke_dart_fn_sync`  
- `src/runtime/runtime.cpp` — `complete_dart_fn`、pending 表  
- `src/wire/demo_api.cpp` — `kCallDartHello`（pool）/ `kCallDartHelloSync`（当前线程）  
- `dart/lib/src/bridge.dart` — `_handleDartFnCall` / `dcb_dart_fn_reply`

---

## 2. 【已绕过】normal / sleepTest 不宜依赖 `spawn_blocking`+FutureAwaiter

- **现象**：早期 wire 用 `co_await spawn_blocking(sleep)` 会挂起不返回。  
- **现状**：`sleepTest` / `ticks` 间隔等改为 `asio::post(thread_pool, ...)`，结果再 `post` 回业务（或直接 `try_post`）。  
- **债**：`Runtime::spawn_blocking` 若仍保留 Future 路径，与 §1 同源，需一并修。

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

**DartFn 反向调用链路已通；唯一硬骨头是 async_simple 协程被 Dart 异步 reply 唤醒的方式。在修好之前，async 入口只能 pool 阻塞等，做不到干净的 io 挂起。**
