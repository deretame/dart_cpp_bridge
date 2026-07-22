# 类 FRB 原生桥：机制说明与 C++ 实现草案

> 通用设计文档：说明 Flutter Rust Bridge（FRB）如何对接原生异步运行时与 Dart 事件循环，以及若用 C++20 按同一模式自研桥时如何落地。
>
> **与具体应用仓库解耦。** 实现可在独立工程中试验，成熟后再决定是否接入业务项目。
>
> C++ 部分为设计草案，默认尚未生产化。

---

## 0. 已锁定决策（讨论结论）

以下条目经讨论定稿，实现与后续修改以此为准。

| 主题 | 决策 |
|------|------|
| Reply 模型 | **长期 reply port + `request_id` 多路复用**（不定一次性 port） |
| 业务协程 | 业务 API **直接返回 `async_simple::coro::Lazy<T>`**，用 `co_await` 同步写法；这是 C++ 侧对应 Rust `async fn → T` 的正常依赖，**不是**桥包装类型 |
| 桥包装 | **禁止**业务返回 `bridge::Future<T>` 或自行 `spawn` 再装回；Dart 侧的 `Future`/`Stream` API 与 wire 调度/回传由 **codegen 生成**，业务只写普通函数/协程（见 §4.2.0） |
| 异步调度器 | **asio 单线程**：`io_context` + **1** 个 run 线程；协程默认无跨线程数据竞争，够用且降复杂度 |
| Blocking 池 | 独立 **`asio::thread_pool`**；任务用 **`asio::post(pool, ...)`**；**禁止**把长时间阻塞丢进单线程调度器 |
| `spawn_blocking` | 运行时**对外提供**类似 tokio `spawn_blocking` 的 API，并用 async-simple 包成可 `co_await` 的 `Lazy`（实现简单）；wire 的 normal 路径与业务手动卸载阻塞都走它 |
| 同步→Dart Future | 仅当业务写**普通同步函数**且需要 Dart `Future` 时，由 wire 调 `spawn_blocking`（类 FRB `wrap_normal`）；业务**不写** `Lazy` |
| 注解属性 | 使用 `[[bridge::*]]`；对外头文件用宏（`BRIDGE_SYNC` 等），**非 codegen 编译时宏为空**，避免 unknown-attribute 告警 / `-Werror` |
| 错误（C++） | wire 边界统一 `catch (const std::exception&)` 再 `catch (...)`，编成错误帧；**异常永不穿出 FFI** |
| 错误（Dart） | 公用库对齐主流用法：**直接抛异常**（`Future` / `Stream` error），不做 `Result` 双模式 |
| 取消 | **不做**任何 CancelToken / 通用 async 取消；Dart 普通 `Future` 本身也不可取消 |
| Stream 与 Dart 关闭 | Dart 关闭订阅后，C++ 侧 `StreamSink::add` **发送失败则静默丢弃**，业务逻辑**照常跑完**；**不**由 Dart 主动打断 C++ 操作 |
| dispose | 仅生命周期：`generation` / 会话失效后 **禁止再 post 进 isolate**（丢弃晚到结果）；这不是 cancel API |
| Codegen 工具链 | **Python 解释器 + libclang 均为远端下载的固定版本**（URL + hash 锁定）；**禁止**使用本机 Python、本机 LLVM/系统 libclang |
| Codegen 冷启动 / 校验 | 每次启动 codegen **先检测**缓存的 Python、libclang 的 **hash**；缺失或与 `versions.lock` 不符 → **重新下载**后再跑 |
| Codegen 触发 | **仅手动**（改 API 头后执行）；与构建**解耦** |
| 构建 / 链接 | **Dart Native Assets hook** 只负责自动编译 C++ 并链接动态库；**不**在 hook 里跑 codegen |
| Hook vs Codegen 工具链 | **不同**：codegen 用远端 Python/libclang；**hook 编译 C++ 仍用本机/CI 的** MSVC、NDK、Xcode 等（不可避免） |
| C++ 依赖分发 | 提供 **依赖库**（内含并 **对外暴露** 钉死版本的 **asio** 与 **async-simple**）；用户工程用 **CMake `FetchContent`** 拉取该库 |
| 用户接入形态 | 提供 **示例工程/模板**（CMake + 示例 `bridge_api` + 示例业务 + 调 codegen 步骤）；用户在此基础上改，而不是从零搭 |
| `StreamSink::add` | **允许**从 `io_context` 线程与 `thread_pool` 线程调用；实现内部串行化/投递，保证线程安全 |
| 生成物是否进 Git | **由用户自定**；库与文档不做强制约定 |

---

## 1. 问题本质

Dart/Flutter 与原生代码（Rust / C++ / …）各自有独立执行模型：

| 侧 | 典型模型 | 特点 |
|----|----------|------|
| Dart | Isolate 事件循环 | 单线程协作；`Future` / `Stream` 挂在该循环上 |
| Rust | 常见为 tokio 等 | 多线程异步 |
| C++ | 常见为 asio 等 | 多线程异步 |

**两边不能共享同一个 event loop。** 桥只做三件事：

1. 把调用从 Dart 安全投递到原生调度器；
2. 在原生侧执行业务（可 async）；
3. 把结果 / 错误 / 流式事件投递回 **发起调用的 Dart Isolate**。

稳定抽象是三通道：

```text
sync       调用线程立刻返回
async RPC  一次请求 → 一次完成（Dart Future）
stream     一次订阅 → 多次事件（Dart Stream）
```

```text
Dart Isolate  ←—— port / FFI / 回调指针 ——→  原生 Runtime
(Future/Stream)                              (tokio / asio / …)
```

---

## 2. FRB 是怎么实现的

以下描述以 **flutter_rust_bridge 2.x** 默认 Handler 为准（概念级，便于对照自研）。

### 2.1 角色划分

| 组件 | 职责 |
|------|------|
| Codegen | 读 Rust API 上的 `#[frb]` 等标记，生成 Dart API + Rust `wire_*` |
| Dart `BaseHandler` | 把一次调用变成 `Future`（或 sync 返回值） |
| Rust `DefaultHandler` | 解码参数、选择调度器、编码结果、发回 port |
| `SimpleThreadPool` | 跑「非 async 的普通 `fn`」 |
| `SimpleAsyncRuntime` | 内部持有 **`tokio::Runtime`**，跑 `async fn` |
| `DartFnHandler` | Rust 回调 Dart 并可 `await` 返回值 |
| Codec（如 SSE） | 参数/返回值二进制布局 |

默认（非 web）结构：

```text
FLUTTER_RUST_BRIDGE_HANDLER
  = SimpleHandler
      + SimpleExecutor
          + SimpleThreadPool      // normal
          + SimpleAsyncRuntime    // tokio::Runtime::new()
      + DartFnHandler             // 反向调用
```

业务侧一般**不必**再 `#[tokio::main]`。  
在 FRB `wrap_async` 路径里，`tokio::spawn` / `Handle::try_current()` 可用，因为 future 已跑在 FRB 创建的 Runtime 上。

### 2.2 Dart → 原生（async RPC）

Dart 侧（`executeNormal` 思路）：

```text
1. Completer completer = Completer()
2. 将 completer 登记到 request_id（长期 reply port 多路复用）
3. FFI: wire_xxx(nativePort, request_id, 编码参数…)
4. return completer.future.then(decode)
```

（FRB 实现细节可能随版本变化；自研桥**定死**长期 port + id，见 §0 / §4.4。）

Rust 侧：

```text
1. wrap_async：解码参数，得到 async 闭包
2. async_runtime.spawn { 业务.await; 编码; sender.send(port) }
3. 消息进入 Dart ReceivePort → 按 id 找到 Completer.complete
```

**Dart 事件循环与 tokio 只通过 isolate port 交接，互不嵌入。**

### 2.3 业务 API 长什么样（重点）

FRB **不要求**业务返回特殊的 `BridgeFuture<T>` / `BridgeStream<T>`。  
业务只写「普通函数」；**Future/Stream 由 codegen + 运行时在外侧包出来**。

| 通道 | Rust 业务签名（概念） | 生成的 Dart |
|------|----------------------|-------------|
| async RPC | `async fn fetch(req) -> Result<Resp>` | `Future<Resp> fetch(req)` |
| stream | `fn ticks(sink: StreamSink<T>) -> Result<()>` | `Stream<T> ticks()` |
| sync | `fn version() -> i32`（`#[frb(sync)]`） | `int version()` |
| normal（同步体、Dart 异步） | 普通 `fn` + `#[frb]`（非 sync） | `Future<...>`，池上跑阻塞体 |

**async：** 就是语言自带的 `async fn` + 普通返回值。  
wire 层负责 `runtime.spawn`、把 `Ok/Err` 编码后 `port.send`。业务**看不到** port。

**normal（类 spawn_blocking）：** 业务写同步函数，例如：

```rust
#[frb]
pub fn sleep_test() -> String {
    std::thread::sleep(std::time::Duration::from_secs(5));
    "Done".to_string()
}
```

FRB 将其丢到 **thread pool** 执行，Dart 仍得到 `Future`——避免阻塞 isolate。  
自研桥对「普通 `T f()` 且非 sync」走同一语义（见 §4.2 / §6.2）。

**stream（官方模式）：**

```rust
// 参数里要一个 StreamSink；可放到任意参数位置
fn f(sink: StreamSink<T>, /* 其它参数 */) -> Result<()> { ... }
// 或
fn f(a: i32, sink: StreamSink<String>) -> Result<()> { ... }
```

- Dart 调用后立刻得到 `Stream<T>`，与 `sink` 已接通。
- Rust 函数本身可以**马上返回**；`StreamSink` 可继续持有，稍后（甚至很久以后）`sink.add` / 结束。
- 日志流、进度、长生命周期事件都靠这个，而不是 `fn f() -> Stream<T>`。

这是自研桥应优先对齐的形状：**业务不包 Future/Stream 包装类型；包装是桥的事。**

### 2.4 三种调度

| API 形态 | 调度 | 说明 |
|----------|------|------|
| `async fn` + `#[frb]` | **tokio**（`wrap_async`） | IO、真正异步；返回普通 `T`/`Result<T>` |
| 普通 `fn` + `#[frb]` | **thread pool**（`wrap_normal`） | 阻塞体；含带 `StreamSink` 的启动函数等 |
| `#[frb(sync)]` | **当前 FFI 线程**（`wrap_sync`） | 立刻返回；必须短小、无阻塞 |
| `#[frb(init)]` | 库初始化时调用 | 全局一次性设置 |

### 2.5 反向调用

**DartFn（Rust → Dart → 再回 Rust）**

```text
Rust await dart_callback(args)
  → 经 dart_handler_port 把闭包调用丢到 Dart Isolate
  → Dart 执行
  → 结果经 port 回 Rust（oneshot 完成）
```

（C++ 草案第一阶段可不实现；见 §9。）

### 2.6 端到端时序

```text
┌────────────── Dart Isolate ──────────────┐
│  await api.doWork(...)                   │
│    Completer + request_id                │
│    FFI(wire, port, id, args)             │
└──────────────────┬───────────────────────┘
                   │
┌──────────────────▼──── 原生 so/dll ──────┐
│  wire_impl                               │
│    wrap_async → tokio::spawn             │
│      业务.await → Post 结果到 port       │
└──────────────────┬───────────────────────┘
                   │
┌──────────────────▼───────────────────────┐
│  ReceivePort → 按 id complete → 继续     │
└──────────────────────────────────────────┘
```

### 2.7 和 `NativeCallable.listener` 的关系

| API | 能力 |
|-----|------|
| `NativeCallable.listener` | 任意线程可调；投回创建它的 Isolate；**仅 void**；必须 `close()` |
| `NativeCallable.isolateLocal` | 同线程；可同步有返回值 |
| `NativeCallable.isolateGroupBound` | 任意线程；group 内同步执行 |

FRB 的 **RPC 主路径**是 **Completer + port**（要返回值/错误）。  
`listener` 适合单向事件（进度、日志），**不能**单独替代整座桥。

---

## 3. C++ 自研桥：目标

用 **C++20** 做与 FRB **同构**的半边：

| 层 | 选型 | 角色 |
|----|------|------|
| IO / 调度 | **asio** `io_context` + **1** 个 run 线程 | 原生事件循环（协程默认跑这里） |
| 阻塞卸载 | **`asio::thread_pool`** + `asio::post`；对外 `spawn_blocking` | normal 路径与业务主动卸载阻塞 |
| 协程 | **async-simple**（经依赖库暴露） | 业务 `async/await`（`Lazy<T>`）；`spawn_blocking` 可包成可 await 的 Lazy |
| C++ 依赖 | **bridge 依赖库**（FetchContent） | 内聚并 re-export **asio** + **async-simple**（版本钉死） |
| Codegen | **远端固定版 Python + 远端固定版 libclang**（均不用本机） | 手动；每次启动 hash 校验，失败则重下 |
| 构建 | **Dart Native Assets hook** | 仅自动编译/链接；C++ 编译器用**本机/CI**工具链 |
| 回 Dart | **长期 reply port + request_id**；（可选）`NativeCallable.listener` | 进 Isolate 的唯一入口 |
| 接入 | **示例模板工程** | 用户 FetchContent + 改示例 API/业务 + 手动 codegen |

非目标（第一阶段）：

- 不把 asio 与 Dart 跑在同一线程；
- 不把调度器做成多线程 worker 池（默认单线程；多线程事件循环非目标）；
- 不把 asio/async-simple/STL **实现细节**当作线协议（协议按逻辑类型编）；
- 不支持任意模板/重载/默认参数作为稳定 API 面；
- **不做** CancelToken / Dart 主动取消 C++ 任务；
- 不做 Dart `Result` 双模式（失败即抛异常）；
- **不**在 `flutter build` hook 里隐式 codegen。

```text
┌──────────────── Dart Isolate ────────────────┐
│  生成的 API：Future / Stream / sync          │
│  长期 ReceivePort + request_id 表            │
└─────────────────────┬────────────────────────┘
                      │ FFI / port / 函数指针
┌─────────────────────▼── bridge.so ───────────┐
│  Codec + HandleTable + Session + Dispatch    │
│         ┌───────────┴───────────┐            │
│         ▼                       ▼            │
│  io_context (1 thread)   asio::thread_pool   │
│  协程 / 非阻塞 IO         spawn_blocking      │
│         │                       │            │
│         └───────────┬───────────┘            │
│                     ▼                        │
│            async-simple 业务                  │
└──────────────────────────────────────────────┘
```

端到端工作流（库用户视角）：

```text
1. 在用户工程中用 CMake FetchContent 拉取 bridge 依赖库
       （库内已带并暴露 asio、async-simple）
2. 从官方示例模板拷贝/生成起步代码（示例 bridge_api + impl + CMake 片段）
3. 改 API 头与业务实现
4. 手动跑 codegen 入口脚本
       → 每次启动：校验缓存 Python / libclang 的 hash
       → 缺失或 hash 不符：远端重新下载固定版本（不用本机）
       → 用该 Python + 该 libclang 解析 → 生成 Dart API + C++ wire
5. flutter run / build
       → Native Assets hook 用本机/CI 的 C++ 工具链编译并链接动态库
6. 业务 Dart 只调用生成的 sync / Future / Stream API
```

硬规则：

1. 单线程 `io_context` 上**禁止**长时间同步阻塞；阻塞必须 `spawn_blocking` / `asio::post(thread_pool)`；
2. 调度线程与 thread_pool 线程都不直接碰 Dart API（除文档保证线程安全的 post）；`StreamSink::add` 可从两池调用，由实现保证安全投递；
3. C++ 异常不穿越 FFI，边界统一编成错误帧；
4. dispose / isolate 销毁时：拒绝再 post；晚到结果静默丢弃（**不是**取消 inflight 业务）。

### 3.1 C++ 依赖库与用户 CMake（形态）

目标：用户**不必**自己找 asio/async-simple 版本、写一堆 Fetch；桥把依赖收成**一个**可 Fetch 的库，并 **PUBLIC 暴露**这两个依赖，业务 `#include` / link 即可。

```cmake
# 用户工程（示例片段，概念）
include(FetchContent)
FetchContent_Declare(native_bridge
  GIT_REPOSITORY https://.../native_bridge.git   # 或依赖库独立 repo
  GIT_TAG        vX.Y.Z                          # 钉死
)
FetchContent_MakeAvailable(native_bridge)

add_library(my_bridge_api ...)
target_link_libraries(my_bridge_api PRIVATE native_bridge::runtime)
# native_bridge::runtime 已 PUBLIC 带上 asio / async-simple
```

- 依赖库内部：用 CMake FetchContent（或等价）拉取**钉死版本**的 asio、async-simple，再 export 给下游。
- 仓库提供 **examples/**（最小 Flutter/Dart 包 + CMake + 示例 API + 一键说明「如何手动 codegen」）；用户复制示例后改，而不是空仓库从零搭。
- **`generated/` 是否提交 Git：不规定**；示例 README 可列出利弊，决定权在用户。

---

## 4. C++ 运行时

### 4.1 Runtime 与 Session

```text
BridgeRuntime
  ├─ asio::io_context
  │     └─ 恰好 1 个 run 线程          // 协程调度器；默认无跨线程竞争
  ├─ asio::thread_pool                 // 仅阻塞任务
  │     └─ asio::post(pool, fn)
  ├─ spawn_blocking(fn) → Lazy<T>      // 对外 API：post 到 pool，完成后回到可 await
  └─ shutdown: stop io_context → join pool（不传播 cancel 给业务）

BridgeSession（每个 Dart Isolate / 每次 init 一份）
  ├─ 长期 reply SendPort（原生侧持有投递端）
  ├─ generation（dispose 时递增）
  ├─ request_id → 进行中调用的登记（Dart 侧 Completer 表对称）
  └─ stream 订阅表（id → StreamSink 内部状态）
```

**为何调度器单线程**

- Flutter 插件场景并发以 IO 等待为主，单线程上的协程数量通常足够；
- 业务少写锁/`strand`，dispose 与 Session 状态更简单；
- **代价**：任何堵在 `io_context` 线程上的同步调用都会卡住整桥 → 必须把阻塞甩到 `thread_pool`。

**`spawn_blocking`（对外 + wire 共用）**

```text
业务或 wire:
  co_await runtime.spawn_blocking([&] {
    // 同步阻塞工作：压缩、同步文件、sleep_test 等
    return value;
  });

实现要点（概念）:
  1. asio::post(thread_pool, 执行 fn)
  2. 完成后把结果送回可被 async-simple 唤醒的路径
  3. 调用方得到 Lazy<T>，在 io_context 协程里 co_await
```

wire 的 **normal** 路径直接 `spawn_blocking` 调用户同步函数，不必再发明第二套池。

生命周期：

1. Dart `Bridge.init()` → 建 Runtime（若尚未）、建 Session、挂长期 ReceivePort；
2. 协程任务进 `io_context`；阻塞任务 `asio::post(thread_pool, …)`；
3. `Bridge.dispose()` → generation++、清空 Dart 侧 Completer/Controller 表、之后原生 `post_result` 若 generation 不匹配则 **丢弃**；再 stop/join（策略可配置，默认先停接收再停线程）。

### 4.2 业务 API 形态（对齐 FRB）

#### 4.2.0 「Future/Stream 外壳由 wire / Dart 生成」指什么

这句话容易含糊，含义是**分层**，不是业务禁止用协程：

| 层 | 职责 | 出现什么 |
|----|------|----------|
| **业务 C++** | 算什么 | `Lazy<T> fetch(...)` / `std::string sleep_test()` / `void f(StreamSink<T>)`；**不**写 Dart 类型，也**不**写 `bridge::Future` |
| **wire（生成的 C++）** | 怎么调度、怎么打成字节发回 | `spawn` / `spawn_blocking`、catch、按 `request_id` post；**不**叫 Future |
| **Dart（生成）** | 给用户 await / listen | `Completer`→`Future`，`StreamController`→`Stream` |

因此：**用户看见的 `Future`/`Stream` API 是生成出来的**；业务只提供普通函数/协程/`StreamSink` 发送端。

#### 4.2.1 `Lazy<T>` 与「桥 Future」不是一类东西

| 类型 | 含义 | 业务 API |
|------|------|----------|
| `async_simple::coro::Lazy<T>` | C++ 协程返回类型，对应 Rust `async fn → T` | **应使用**；公开 API 依赖 async-simple 正常 |
| `bridge::Future<T>`（业务自己 spawn 再返回） | 桥包装，手写别扭 | **禁止** |

C++ 没有 Rust 的 `async fn f() -> T` 语法糖，协程函数签名**必须**是 `Lazy<T> f()` 才能 `co_await`。  
若强行在头文件写成「同步 `T f()` 声明、实现里再变协程」，会对不齐，并容易退化成回调模型——**不可取**。

#### 4.2.2 反例（已废弃）

```cpp
// 不好：业务被迫返回 bridge::Future<T>，手写别扭，codegen 也难
bridge::Future<FetchResponse> fetch(FetchRequest req) {
  return runtime_->spawn_on_asio([...]() -> Lazy<FetchResponse> { ... });
}
```

#### 4.2.3 正例：业务只写普通签名，桥在外侧调度

```cpp
// ---- 业务（人手写，或以后由 impl 文件提供）----

// (1) 真异步：协程 + Lazy（业务依赖 Lazy 完全正常）
async_simple::coro::Lazy<FetchResponse>
fetch(FetchRequest req) {
  auto body = co_await http_get(req.url, req.timeout_ms);
  co_return FetchResponse{std::move(body)};
}

// (2) normal：同步函数，Dart 仍要 Future —— 业务不写 Lazy
//     wire 用线程池执行（类 FRB wrap_normal）
std::string sleep_test() {
  std::this_thread::sleep_for(std::chrono::seconds(5));
  return "Done";
}

// (3) stream：参数带 StreamSink；函数可立刻返回，sink 可长期持有
void create_log_stream(StreamSink<std::string> sink) {
  Logger::instance().subscribe(std::move(sink));
}

void download(std::string url, std::string path, StreamSink<Progress> sink) {
  // 可在此 spawn 协程；下载循环里 sink.add(progress)；结束 sink.end() / sink.add_error(...)
  // Dart 若已取消订阅：add 静默失败，本函数/协程仍跑到自然结束
}

// (4) sync
int32_t bridge_version() { return 1; }
```

```cpp
// ---- wire / 生成代码（业务不写这些）----

// async 协程：
void wire_fetch(Session* s, uint64_t request_id, bytes args) {
  auto req = decode<FetchRequest>(args);
  auto gen = s->generation();
  runtime().spawn_on_asio([s, gen, request_id, req = std::move(req)]() -> Lazy<> {
    try {
      auto result = co_await fetch(std::move(req));
      s->try_post_ok(gen, request_id, result);  // generation 不匹配则丢弃
    } catch (const std::exception& e) {
      s->try_post_err(gen, request_id, e.what());
    } catch (...) {
      s->try_post_err(gen, request_id, "unknown");
    }
  });
}

// normal（同步体 → spawn_blocking / thread_pool → 一次 reply）：
void wire_sleep_test(Session* s, uint64_t request_id, bytes args) {
  auto gen = s->generation();
  runtime().spawn_on_asio([s, gen, request_id]() -> Lazy<> {
    try {
      // 内部：asio::post(thread_pool, ...)；可 co_await
      auto out = co_await runtime().spawn_blocking([] { return sleep_test(); });
      s->try_post_ok(gen, request_id, std::move(out));
    } catch (const std::exception& e) {
      s->try_post_err(gen, request_id, e.what());
    } catch (...) {
      s->try_post_err(gen, request_id, "unknown");
    }
  });
}

// stream：
void wire_create_log_stream(Session* s, uint64_t stream_id, bytes args) {
  auto sink = StreamSink<std::string>::from_session(s, stream_id);
  try {
    create_log_stream(std::move(sink));
    // 不在这里 end；由业务/持有方决定何时 end
  } catch (const std::exception& e) {
    s->try_post_stream_error(stream_id, e.what());
  } catch (...) {
    s->try_post_stream_error(stream_id, "unknown");
  }
}
```

| 谁 | 职责 |
|----|------|
| **业务** | 普通协程（`Lazy<T>`）/ 普通函数；stream 只依赖 `StreamSink<T>` 的 `add`/`end`/`error` |
| **wire（生成）** | 解码、调度（`io_context` / `spawn_blocking`）、port 封成 Sink 或按 id reply、**统一 catch**、编码回传 |
| **Dart（生成）** | 用户可见的 `Future`/`Stream` API；失败 **throw**；业务 C++ **不出现** Dart 的 `Future`/`Stream` 类型名 |

`StreamSink<T>` 是桥提供的**小工具类型**（类似 FRB 的 `StreamSink`），不是业务返回值。  
实现上内部是「能安全 post 到某 session/stream_id 的句柄」，可 `shared_ptr` 式共享，便于长期持有。

**线程与关闭语义：**

- **`add` / `end` / `error` 允许**在 `io_context` 线程与 `thread_pool` 线程调用（业务在 `spawn_blocking` 里上报进度合法）。
- 实现必须内部同步或投递到统一发送路径，保证多线程 `add` 不损坏状态、不双重 post 竞态炸进程。
- **Dart 关闭订阅后：** `add` 发现订阅已失效 → **静默返回**（不抛给业务、不打断业务）。

### 4.3 async-simple 与两套执行器

- **业务协程体**里用 async-simple（`co_await` IO 等），签名为 `Lazy<T>`；默认跑在 **单线程 `io_context`**；
- **启动**只发生在 wire：`runtime.spawn_on_asio(业务协程)`；
- 业务函数**不要**自己 `return runtime.spawn(...)` 再包一层桥 Future；
- 业务内部若有阻塞段：`co_await runtime.spawn_blocking(...)`（与 wire normal 同一套）；
- **normal 路径**：业务仍是普通同步函数；wire 用 `spawn_blocking` 调用，**不要**逼业务把签名改成 `Lazy`。

### 4.4 通道判定与实现视角

**判定表（codegen / 手写 wire 共用）：**

| 条件（优先级从上到下） | kind | 调度 | Dart |
|------------------------|------|------|------|
| 参数含 `StreamSink<T>` | `stream` | 调用业务（可马上返回）；事件经 sink | `Stream<T>`（sink 不出现在 Dart 参数里） |
| `BRIDGE_ASYNC` / 返回 `Lazy<T>` | `async` | **单线程** `io_context` 上 `co_await` 业务 | `Future<T>` |
| `BRIDGE_SYNC` | `sync` | 当前 FFI 线程 | 同步返回值 |
| 普通 `T foo(...)` 且非 sync | `normal` | **`spawn_blocking` → `asio::thread_pool`** | `Future<T>` |

**(A) sync**  
FFI 当前线程直接调业务 → 同步返回 buffer；边界同样 catch 后变成错误返回（或约定 sync 只允许不败失败，但库侧仍应 catch，避免 abort）。

**(B) async RPC（含 normal）**

```text
Dart:  长期 ReceivePort + request_id → Completer 表
wire:  spawn（asio 或 blocking）{ 调业务; try_post 一次结果 }
业务:  Lazy<T> 或 普通 T；无 port / 无 request_id 参数
失败:  错误帧 → Dart Completer.completeError → 调用方看到抛出的异常
```

**(C) stream（FRB StreamSink 模式）**

```text
Dart:  StreamController；subscription_id；listen / cancel 只影响是否再收事件
wire:  构造 StreamSink 注入业务
业务:  fn(StreamSink<T>, ...)；add / end / error
Dart cancel:  仅使 sink 失效；C++ add 静默丢弃；业务不中断
```

可选补充：全局 void 事件可用 `NativeCallable.listener`；**主推仍是 StreamSink + port**，与 FRB 一致、能带类型载荷与错误。

### 4.5 不做取消；句柄与错误

#### 取消（明确不做）

- **不提供** CancelToken、不提供 Dart `cancel(request_id)` 去打断 C++。
- 理由：Dart 普通 `Future` 本身不支持取消；做原生取消链路成本高、与主流 async 用法不一致，吃力不讨好。
- Dart 侧若不再 `await` 或 discard Future：等价于**忽略结果**；C++ 仍跑完，`try_post` 时若 Completer 已无或 session 已 dispose 则丢弃。
- **Stream：** 唯一与「关闭」相关的行为是订阅结束 → sink 不再投递；**仍然不**取消 C++ 侧下载/循环等。

#### 句柄

- `uint64_t` + 进程内表；Dart `Finalizer` → `release`；dispose 清空表。

#### 错误

**C++ wire 边界（所有生成路径统一）：**

```cpp
try {
  // 调业务
} catch (const std::exception& e) {
  // code + e.what() → 错误帧
} catch (...) {
  // code + "unknown" → 错误帧
}
```

- 线格式：`ok + payload` / `err + code + message`。
- **Dart：** `completeError` / `StreamController.addError`，对用户表现为 **抛异常**。
- 公用库不暴露 `Result` 双 API；业务若用 `tl::expected` 等，在 wire 内映射为 ok 帧或 err 帧（最终 Dart 仍是值或抛错）。

---

## 5. 线协议（草案）

```text
magic:       u32   // 如 'BRG1'
version:     u16
msg_type:    u8    // Request / Response / StreamData / StreamEnd / Error
flags:       u8
request_id:  u64   // RPC 与 stream 订阅共用 id 空间或分子空间，实现时定一种
method_id:   u32   // codegen 分配
payload_len: u32
payload:     bytes
```

说明：

- **不需要** `Cancel` 消息类型（无取消协议）。
- 端序、对齐、string 长度前缀等在实现 Phase 1 时写死并测跨平台。

类型白名单（先做这些）：

| C++ 桥接面 | Dart |
|------------|------|
| 标量 `bool` / 定宽整数 / `double` | 对应标量 |
| `std::string` | `String`（UTF-8） |
| `std::vector<uint8_t>` | `Uint8List` |
| 聚合 `struct` | class |
| `enum class : int32_t` | enum |
| `Handle<T>` | 不透明包装 |
| 参数 `StreamSink<T>` | （该函数）`Stream<T>`；**sink 不出现在 Dart 参数列表** |
| 协程返回 `Lazy<T>`（内层 `T`） | `Future<T>` |
| 普通同步 `T` 且 kind=normal | `Future<T>` |
| `BRIDGE_SYNC` 的 `T` | 同步 `T` |

说明：Dart 的 `Future`/`Stream` **不**对应 C++ 业务返回类型里的桥包装类，而对应 **函数形态**（`Lazy` / normal / `StreamSink` / sync）。

禁止：裸指针、`std::function`、随意模板、虚接口导出、重载、默认参数。  
协议按 **逻辑类型** 编，不要直接拿 C++ ABI 当线格式。

---

## 6. Codegen：Python + 固定版本 libclang

### 6.0 与构建解耦

| 步骤 | 谁触发 | 做什么 |
|------|--------|--------|
| **Codegen** | **开发者手动**（API 头变更后） | 固定版 Python + 固定版 libclang 解析头 → 生成 Dart + C++ wire |
| **Build** | `flutter run` / `build` → **Native Assets hook 自动** | 编译已有 C++（含 generated wire + 用户 impl）→ 链接动态库 |

- Hook **只**构建与链接，**绝不**隐式调用 codegen（可复现、可调试、CI 清晰）。
- **Hook 的 C++ 工具链 ≠ Codegen 工具链**：编 so/dll 仍用本机或 CI 的 MSVC / Android NDK / Apple clang；文档与示例必须写明，避免误解为「全链路零 C++ 编译器」。
- Codegen **工具链完全自举，两样都不能用本机自带的**：
  1. **Python 解释器**：按 `versions.lock` 从远端下载固定版本（及必要标准库/嵌入式布局），缓存到仓库或用户缓存目录；入口**只用**这份解释器，**不**调用 PATH 上的本机 Python。
  2. **libclang**：同样远端固定版本 + hash，**不**使用本机 LLVM / Xcode clang / 系统包。
- **每次**启动 codegen：**先对缓存的 Python 与 libclang 做 hash 校验**；文件缺失、hash 与 lock 不一致 → **删除坏缓存并重新下载**，通过后再解析。不信任「上次下过就永远可用」。
- 目标：「跑官方 codegen 入口 → 校验/拉工具链 → 解析生成」；开发机无前置 LLVM/Python 版本要求（需网络与解压能力）。
- **生成物是否纳入 Git：库方不规定**，由使用方自行选择。

### 6.1 原则

- 只解析 **窄桥接头**（如 `bridge_api.h` + `bridge_types.h`）；
- 业务 API **可以**包含 `Lazy<T>`（async-simple 头）；asio 实现、wire 细节仍不进业务 AST 也可拆文件；
- 结构体 / 函数用注解宏（见 §6.1.1）；
- **stream 不靠返回 `Stream<T>`**，靠参数列表里出现 `StreamSink<T>`（与 FRB 相同）；
- 解析用 compile flags 与真编译一致（`-std=c++20`、`-I`、宏）；
- **Python 与 libclang 均仓库锁定 + 远端拉取 + hash 校验；明确禁止回落到本机安装。**

#### 6.1.1 注解宏（减少非标准告警）

`[[bridge::sync]]` 等自定义属性在 Clang/GCC/MSVC 上通常**可编译**（忽略未知属性），但可能 **warning**，在 `-Werror` 下会失败。  
作为公用库，**正常编译路径应零告警**：

```cpp
// bridge_annotate.h
#pragma once

#if defined(BRIDGE_CODEGEN)
#  define BRIDGE_SYNC   [[bridge::sync]]
#  define BRIDGE_ASYNC  [[bridge::async]]
#  define BRIDGE_EXPORT [[bridge::export]]
#else
#  define BRIDGE_SYNC
#  define BRIDGE_ASYNC
#  define BRIDGE_EXPORT
#endif
```

- **业务编译 / 用户集成：** 不定义 `BRIDGE_CODEGEN`，宏展开为空。
- **codegen：** 以 `-DBRIDGE_CODEGEN` 喂给 libclang，AST 中可见 `[[bridge::sync]]` 等。

### 6.2 API 面示例（对齐 FRB）

```cpp
// bridge_api.h — codegen 唯一入口（概念）
#pragma once
#include "bridge_annotate.h"
#include "bridge_types.h"  // StreamSink<T> 等
#include <async_simple/coro/Lazy.h>

namespace demo::api {

struct BRIDGE_EXPORT FetchRequest {
  std::string url;
  int32_t timeout_ms;
};

struct BRIDGE_EXPORT FetchResponse {
  int32_t status;
  std::vector<uint8_t> body;
};

struct BRIDGE_EXPORT Progress {
  int64_t received;
  int64_t total;
};

// sync → Dart: int bridgeVersion()
BRIDGE_SYNC
int32_t bridge_version();

// async：Lazy + 协程体 → Dart: Future<FetchResponse> fetch(...)
// 依赖 Lazy 是正常的；不要写成业务返回 bridge::Future
BRIDGE_ASYNC
async_simple::coro::Lazy<FetchResponse> fetch(FetchRequest req);

// normal：同步函数 → Dart: Future<String> sleepTest()
// wire：spawn_blocking，业务无 Lazy
std::string sleep_test();

// stream：参数带 StreamSink → Dart: Stream<Progress> download(url, path)
void download(std::string url, std::string path, StreamSink<Progress> sink);

// 可长期持有 sink → Dart: Stream<String> createLogStream()
void create_log_stream(StreamSink<std::string> sink);

}  // namespace demo::api
```

Codegen 识别规则：

| 条件 | 生成 |
|------|------|
| 参数含 `StreamSink<T>` | Dart `Stream<T>`；C++ wire 注入 sink |
| `BRIDGE_ASYNC` 或返回 `Lazy<T>` | Dart `Future<T>`；wire asio `spawn` + 单次 reply |
| `BRIDGE_SYNC` | Dart 同步调用 |
| 仅普通 `T foo(...)` 且非 sync | Dart `Future<T>`；wire **blocking 池**（类 FRB normal） |

### 6.3 流水线

```text
手动: ./codegen 或 codegen.bat 等入口（不要直接用本机 python）
    → 读 versions.lock
    → 校验缓存 Python 的 hash（无文件或失败 → 远端重下固定版）
    → 校验缓存 libclang 的 hash（无文件或失败 → 远端重下固定版）
    → 用「下载的 Python」+「下载的 libclang」跑 clang.cindex
    → 解析 bridge_api.h（-DBRIDGE_CODEGEN + 固定 flags）
    → 过滤标记 / 签名推断 → IR (JSON)
    → Dart 生成物 + C++ wire/dispatch + schema
       （是否 git add generated/：用户自定）

之后: flutter build / run
    → hook/build.dart（Native Assets）
    → 使用本机/CI C++ 编译器编译 C++、链接 so/dll/dylib
    → 不跑 codegen
```

### 6.4 IR 示例

```json
{
  "version": 1,
  "functions": [
    {
      "name": "fetch",
      "method_id": 1001,
      "kind": "async",
      "args": [
        {"name": "req", "type": {"kind": "struct", "name": "FetchRequest"}}
      ],
      "returns": {"kind": "struct", "name": "FetchResponse"}
    },
    {
      "name": "sleep_test",
      "method_id": 1004,
      "kind": "normal",
      "args": [],
      "returns": {"kind": "string"}
    },
    {
      "name": "download",
      "method_id": 1002,
      "kind": "stream",
      "stream_item": {"kind": "struct", "name": "Progress"},
      "args": [
        {"name": "url", "type": {"kind": "string"}},
        {"name": "path", "type": {"kind": "string"}}
      ],
      "sink_param": "sink",
      "returns": {"kind": "void"}
    },
    {
      "name": "create_log_stream",
      "method_id": 1003,
      "kind": "stream",
      "stream_item": {"kind": "string"},
      "args": [],
      "sink_param": "sink",
      "returns": {"kind": "void"}
    }
  ]
}
```

注意：IR 里 **没有** `returns: Future<...>` / `returns: Stream<...>`；  
`kind: async|normal|stream|sync` +（stream 时）`stream_item` 就够生成 Dart 外壳。

---

## 7. Dart 侧最小封装

```dart
abstract final class NativeBridge {
  static Future<void> init();
  static void dispose();

  static T invokeSync<T>(/* method + args */);
  static Future<T> invokeAsync<T>(int methodId, Uint8List args);
  static Stream<T> invokeStream<T>(int methodId, Uint8List args);
}
```

约定：

- **一个 Session 一条长期 ReceivePort**；所有 RPC/stream 事件进同一 port，用 `request_id` / `stream_id` 分发。
- async：`request_id` → `Completer` 表；完成时 `complete` / `completeError`（**错误即异常**）。
- stream：`subscription_id` → `StreamController`；`Done` / `Error`；用户 `cancel` 只 `close` controller 并通知原生「sink 失效」，**不**发取消业务指令。
- dispose：清空表、generation 失效；之后入站消息丢弃。
- 全局 void 事件可选用 `NativeCallable.listener`，生命周期与 `close()` 必须闭环。

---

## 8. FRB vs C++ 草案对照

| 维度 | FRB 2.x 默认 | C++ 草案 |
|------|----------------|----------|
| API 标记 | `#[frb]` 等 | `BRIDGE_*` → codegen 时 `[[bridge::*]]` |
| 解析 / 生成 | FRB codegen | **手动**；**远端固定版 Python + 远端固定版 libclang**（均不用本机） |
| 构建集成 | 视项目 | **Native Assets hook** 只编链接，不 codegen；C++ 编译器用本机/CI |
| C++ 依赖 | 用户自管 | **FetchContent 依赖库**，PUBLIC 暴露 asio + async-simple |
| 接入 | — | **示例模板**，用户改示例 |
| Async 运行时 | **tokio** | **asio `io_context` 单线程** |
| 阻塞卸载 | thread pool / spawn_blocking | **`asio::thread_pool` + `spawn_blocking`→Lazy** |
| StreamSink 线程 | 实现相关 | **允许** io 线程与 pool 线程 `add` |
| 协程 | Rust `async fn` | 业务 **`Lazy<T>`**（async-simple）；wire 负责 spawn |
| 普通同步→Future | thread pool（normal） | wire `spawn_blocking`（kind=`normal`） |
| 业务 async 签名 | `async fn f() -> T` | `Lazy<T> f()`（**不**返回桥 Future 包装） |
| 业务 stream 签名 | `fn f(sink: StreamSink<T>)` | `void f(StreamSink<T> sink, ...)` |
| Dart Future | Completer + port | **长期 port + request_id** |
| 流 | `StreamSink` 参数 | 同构；Dart 关闭后 add 静默丢弃 |
| 取消 | （FRB 另有能力，本草案不对齐） | **明确不做** |
| Dart 错误 | 多为异常 | **统一抛异常** |
| 反向调用 | DartFn | 后期可选；非 Phase 1 |

要抄的不是语言，而是：

1. **业务 API 保持「普通函数 / 普通协程」**；Future/Stream 外壳在 wire/Dart 生成；  
2. stream 用 **`StreamSink` 入参**，可长期持有，函数可先返回；  
3. Handler = 调度器 + 回 port；  
4. wire：解码 → 调度 → **catch** → 编码 → send；  
5. 句柄与 dispose（丢弃晚到 post）；codegen 保证两侧一致。

---

## 9. 建议落地顺序（独立工程）

### Phase 1 — 手写骨架（无 codegen）

- Runtime：`io_context` **单线程** + `asio::thread_pool` + `spawn_blocking`→Lazy + shutdown
- Session：长期 port、request_id、generation
- 四个 API：`version` sync、`fetch`/`add` async（`Lazy`）、`sleep_test` normal、`ticks` stream
- Dart：`invokeSync` / `invokeAsync` / `invokeStream`；错误 throw
- wire 全路径 `catch` + dispose 后丢弃 post
- Stream：Dart 关订阅后 C++ 继续跑、add 静默失败
- 至少两个平台各跑通（例如 Windows + Android）

**验收建议：** 并发大量 async、dispose 竞态、热重启、stream 中途关订阅、normal/`spawn_blocking` 不堵 `io_context` 线程。

### Phase 2 — 手动 codegen + 依赖库骨架

- `versions.lock`：钉死 **Python** 与 **libclang** 的版本、URL、hash（按 OS/arch）
- 入口：**每次** hash 校验，失败则重下；**拒绝**本机 Python / LLVM
- 解析窄头 → IR → 生成 Dart + wire
- CMake 依赖库：FetchContent 拉 asio/async-simple，**export** 给下游；最小 `examples/` 模板
- CI：同一套 codegen 下载逻辑 + 可选生成物 diff（是否提交 generated 不强制）
- **仍不**把 codegen 塞进 hook

### Phase 3 — Native Assets + 生产能力

- `hook/build.dart`：按 target 用**本机/CI C++ 工具链**编译、链接（**仅 build**）
- 错误码表、Handle + Finalizer；`StreamSink` 多线程 add 压测
- 拷贝与大 blob 策略
- 压测：并发、热重启、dispose 竞态、listener 泄漏（若使用）
- **不含** CancelToken / 通用取消

### Phase 4 — 再谈接入业务

- 仅新模块 / 逐步迁移 / 或长期独立库  
- **未到 Phase 3 前，不替换任何已有 FRB/生产桥**

---

## 10. 独立工程目录建议

```text
native_bridge/                 # 或拆成 runtime 库 + examples 仓库
├── cmake/
│   └── NativeBridge*.cmake    # FetchContent 拉 asio / async-simple，export target
├── include/
│   ├── bridge_annotate.h
│   ├── bridge_types.h         # StreamSink 等
│   └── bridge_runtime.h       # spawn_blocking；asio/async-simple 经 target 暴露
├── src/
│   ├── runtime/               # io_context(1) + thread_pool + session
│   ├── codec/
│   └── ffi_entry.cc
├── codegen/                   # 仅手动
│   ├── versions.lock          # Python + libclang：版本 / URL / hash
│   ├── bootstrap.ps1/.sh      # 校验 hash → 必要时重下 → 调解析（不用本机 python）
│   ├── parse_libclang.py
│   └── templates/             # 生成物模板
├── examples/                  # 用户起步模板（CMake + 示例 API + 说明）
│   ├── CMakeLists.txt         # FetchContent 本库 + 示例 target
│   ├── bridge_api.h
│   ├── api_impl/
│   └── README.md              # 手动 codegen 步骤；generated 是否入库自定
├── hook/
│   └── build.dart             # Native Assets：本机 C++ 工具链编链接
├── dart/
│   └── lib/
└── README.md
```

---

## 11. 风险（短表）

| 风险 | 缓解 |
|------|------|
| 工作量接近「迷你 FRB」 | 严守阶段；先手写 4 类 API |
| 单线程调度被阻塞卡死 | 文档强调；normal/`spawn_blocking`；禁止在 io 线程同步 sleep/重 CPU |
| 本机 Python/LLVM 版本漂移 | 远端钉死 + **每次启动 hash 校验**，失败重下；禁用 PATH 回落 |
| 缓存被篡改/损坏 | 同上，hash 不符即重下 |
| hook 与 codegen 耦死 | hook 只 build；codegen 仅手动 |
| 跨平台 hook 工具链 | Phase 3；本机 C++ 编译器缺失时错误信息要明确 |
| 用户不会接 asio | FetchContent 依赖库 + **examples 模板** |
| dispose 与 post 竞态 | **generation** + dispose 后丢弃（不做业务 cancel） |
| 未知属性告警 / `-Werror` | `BRIDGE_*` 宏，非 codegen 为空 |
| listener 未 close | 包装强制 close；文档与 lint |
| 异常出 FFI | 边界 `catch` + `catch (...)`；策略全库一致 |
| 误做全套取消 | 文档写死不做；review 拒绝 CancelToken API |

---

## 12. 参考

- Flutter Rust Bridge：<https://cjycode.com/flutter_rust_bridge/>
- Dart `NativeCallable.listener`：<https://api.dart.dev/dart-ffi/NativeCallable/NativeCallable.listener.html>
- asio、async-simple、libclang：各自上游文档

本地若已缓存 FRB 源码，可对照：

- `handler/implementation/handler.rs` — `DefaultHandler`
- `handler/implementation/executor.rs` — `execute_async` / `execute_normal`
- `rust_async/io.rs` — `SimpleAsyncRuntime`（tokio）
- Dart `BaseHandler.executeNormal` — Completer + port

---

## 13. 一句话

- **FRB**：业务写普通 `async fn` / 普通 `fn`（normal 池）/ `fn(StreamSink<T>, …)`；**不**返回桥专用 Future/Stream 类型。Dart 的 Future/Stream 由运行时 + codegen 用 **port** 接出来；async 跑在内置 **tokio** 上。  
- **C++ 草案**：业务写 `Lazy<T>` / 同步 `T` / `StreamSink`；**单线程 asio** 跑协程，**`asio::thread_pool` + `spawn_blocking`** 卸阻塞；wire **统一 catch**；**长期 port + request_id**；Dart **抛异常**。  
- **工具链**：codegen = **手动** + **远端固定版 Python/libclang**（每次 hash 校验，失败重下，不用本机）；hook = **本机 C++ 编译器**只编链接。  
- **依赖**：CMake 库 FetchContent，**暴露 asio + async-simple**；**examples 模板**给用户改。  
- **StreamSink**：io 线程与 pool 线程均可 `add`；Dart 关流后静默丢。  
- **不做取消**；dispose 用 generation 丢弃晚到 post。生成物是否进 Git **用户自定**。  
- **listener** 替不了 RPC / 类型化 Stream。

先在独立仓库把 Phase 1–2 做稳，再考虑与任何业务工程集成。
