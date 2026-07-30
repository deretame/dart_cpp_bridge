---
title: 纯 C 桥接 API
description: 为纯 C 调用方提供的 C99 兼容 API — 编解码、异步操作、调用 Dart 函数
---

本页介绍 `cbridge.h` 和 `dcb_codec.h` 提供的**纯 C API**，以及配合 C++ 协程使用的 `cbridge_wait.hpp`。

你可以用它们做三件事：

- 用 `dcb_codec.h` 编解码 wire 数据，与 C++ 侧 `ByteWriter` / `ByteReader` 二进制兼容
- 用 `dcb_invoke_dart_fn` 从任意线程调用已注册的 Dart 回调
- 用 `dcb_async_*` 配合 `cbridge_wait.hpp` 让 C++ 协程非阻塞等待外部 C 异步操作完成

如果你的代码是**纯 C 项目、只能导出 C 符号，或者不想把 async-simple / asio / C++20 协程引入调用方**，就应该用这章的 C 入口。

> 其他语言 runtime（如 Python `ctypes`、Rust FFI、Go `cgo`）也可以把这套 C API 当作最小接入点。

## 什么时候用纯 C API

以下情况选择这套入口：

- **你的代码是纯 C 项目，或只能导出 C 符号**：C 编译单元没有 `co_await`、模板和 `Executor`，无法直接使用 bridge 的 C++ 协程入口。
- **你从其他语言 runtime 调用 bridge**：Python `ctypes`、Rust FFI、Go `cgo` 等只能绑定到 C ABI。
- **你希望调用方保持零 C++ 依赖**：C 端只编译 C 头文件，bridge 内部的 async-simple + asio 对调用者不可见。

:::caution[C 端不能等待]
**C 代码本身不能 `co_await` 这些异步操作。** `dcb_async_*` 的等待端是 C++ 协程（通过 `cbridge_wait.hpp`），C 端只负责创建、完成或取消操作。
:::

纯 C API 是**零 C++ 依赖的最低公分母入口**：

```text
任何 C/C++ 运行时（不管用什么协程/事件循环）
    │
    │  dcb_codec.h   — 编解码参数/返回值
    │  dcb_async_*   — 异步操作原语
    │  dcb_invoke_dart_fn — 调用 Dart 回调
    ▼
bridge 内部（C++ 协程管道，调用者无需知道）
    │
    ▼
Dart Isolate 执行回调 → 结果通过 callback 返回
```

## 头文件

```c
#include "dart_cpp_bridge/dcb_codec.h"  // wire payload 编解码
#include "dart_cpp_bridge/cbridge.h"    // 异步操作原语 + 调用 Dart 回调
// 纯 C99，不引入任何 C++ 头文件
```

## API 一：纯 C 编解码

`dcb_codec.h` 提供与 C++ 侧 `ByteWriter` / `ByteReader`（codec.hpp）**二进制兼容**的纯 C99 编解码器。

### 支持的类型

| C 类型 | wire 编码 | 说明 |
|--------|-----------|------|
| `int32_t` | 4 bytes LE | bool 也用这个（0/1） |
| `uint32_t` | 4 bytes LE | |
| `int64_t` | 8 bytes LE | |
| `uint64_t` | 8 bytes LE | |
| `double` | 8 bytes LE (IEEE 754) | |
| `const char*` | u32 len + UTF-8 bytes | 唯一变长类型 |
| 数组 | u32 count + N 个元素 | 元素可以是上表任意基本类型 |

数组元素支持：`i32`、`u32`、`i64`、`u64`、`f64`、`str`。同一个数组内元素类型必须一致（由调用者保证）。

不支持 struct / 指针 / 嵌套容器——复杂数据序列化为 string（JSON、protobuf 等）即可。

### Writer

动态增长缓冲区，初始 64 字节，按需倍增。内部 `malloc`/`realloc`，用完后 `dcb_writer_free` 释放。

```c
dcb_writer w;
dcb_writer_init(&w);

dcb_write_str(&w, "hello");     // u32 strlen + UTF-8 bytes
dcb_write_i32(&w, 42);          // 4 bytes LE
dcb_write_f64(&w, 3.14);        // 8 bytes LE
dcb_write_u64(&w, 123456789ULL); // 8 bytes LE

// 数组：先写 count，再循环写元素
dcb_write_arr_begin(&w, 3);
dcb_write_i32(&w, 10);
dcb_write_i32(&w, 20);
dcb_write_i32(&w, 30);

// 使用 w.data / w.len 作为 payload
dcb_invoke_dart_fn(session_id, fn_id, w.data, w.len, callback, userdata);

dcb_writer_free(&w);  // 释放内部缓冲区
```

完整 Writer API：

```c
void dcb_writer_init(dcb_writer* w);
void dcb_writer_free(dcb_writer* w);
void dcb_write_u32(dcb_writer* w, uint32_t v);
void dcb_write_u64(dcb_writer* w, uint64_t v);
void dcb_write_i32(dcb_writer* w, int32_t v);
void dcb_write_i64(dcb_writer* w, int64_t v);
void dcb_write_f64(dcb_writer* w, double v);
void dcb_write_len_bytes(dcb_writer* w, const void* p, uint32_t n);  // u32 len + data
void dcb_write_str(dcb_writer* w, const char* s);                    // inline: strlen + len_bytes
void dcb_write_arr_begin(dcb_writer* w, uint32_t count);             // inline: u32 count
```

### Reader

零拷贝读取器——不 `malloc`，不持有数据所有权。越界读取时进入错误状态（后续返回 0），通过 `dcb_reader_valid` 检查。

:::caution[必须按写入顺序读取]
Reader 是一个**顺序游标**，没有随机访问能力。写入顺序是 `str → i32 → f64`，读取时也必须按 `str → i32 → f64` 的顺序依次调用。跳过或乱序读取会导致后续所有字段解析错误（偏移错位）。类型和顺序必须与写入侧完全一致。
:::

```c
// data / data_len 来自 dcb_invoke_dart_fn 的 callback 参数
dcb_reader r;
dcb_reader_init(&r, data, data_len);

uint32_t slen;
const char* s = dcb_read_str(&r, &slen);  // 零拷贝，指向 data 内部
int32_t v = dcb_read_i32(&r);
double d = dcb_read_f64(&r);

// 读数组
uint32_t n = dcb_read_arr_begin(&r);
for (uint32_t i = 0; i < n; i++) {
    int32_t elem = dcb_read_i32(&r);
    // ...
}

if (!dcb_reader_valid(&r)) {
    // 数据格式不匹配，读取越界
}
```

:::note
`dcb_read_str` 返回的指针**不保证 NUL 结尾**，长度为 `*out_len`。如需 C 字符串，自行拷贝并追加 `'\0'`。
:::

完整 Reader API：

```c
void dcb_reader_init(dcb_reader* r, const uint8_t* data, uint32_t len);
int  dcb_reader_valid(const dcb_reader* r);  // 1=正常, 0=曾越界
uint32_t dcb_read_u32(dcb_reader* r);
uint64_t dcb_read_u64(dcb_reader* r);
int32_t  dcb_read_i32(dcb_reader* r);
int64_t  dcb_read_i64(dcb_reader* r);
double   dcb_read_f64(dcb_reader* r);
const uint8_t* dcb_read_len_bytes(dcb_reader* r, uint32_t* out_len);  // 零拷贝
const char*    dcb_read_str(dcb_reader* r, uint32_t* out_len);        // inline cast
uint32_t       dcb_read_arr_begin(dcb_reader* r);                     // inline: u32 count
```

### 与 C++ codec 的对应关系

| 纯 C (`dcb_codec.h`) | C++ (`codec.hpp`) | wire 格式 |
|---|---|---|
| `dcb_write_i32` | `ByteWriter::i32()` | 4 bytes LE |
| `dcb_write_u32` | `ByteWriter::u32()` | 4 bytes LE |
| `dcb_write_i64` | `ByteWriter::i64()` | 8 bytes LE |
| `dcb_write_u64` | `ByteWriter::u64()` | 8 bytes LE |
| `dcb_write_f64` | `ByteWriter::f64()` | 8 bytes LE |
| `dcb_write_str` | `ByteWriter::str()` | u32 len + bytes |
| `dcb_write_arr_begin` + 循环 | `ByteWriter::vec()` 头 | u32 count + N elements |
| `dcb_read_str` | `ByteReader::str()` | 零拷贝 |

两侧编码完全一致，纯 C 侧写的数据 C++ 侧能直接读，反之亦然。

## API 二：异步操作原语

这组函数用于在 **C 端**创建、完成或取消一个异步操作。C++ 协程侧通过 `cbridge_wait.hpp` 中的 `dcb::async_wait()` 非阻塞等待，期间不占用 bridge 线程。

### 函数签名

```c
uint64_t dcb_async_create(void);                                    // 创建操作
void dcb_async_complete(uint64_t op_id, const uint8_t* data, uint32_t len);  // 成功完成
void dcb_async_fail(uint64_t op_id, const char* error);             // 失败
void dcb_async_cancel(uint64_t op_id);                              // 取消
```

:::note[一次性操作]
`dcb_async_create()` 返回的 `op_id` 是一次性的。调用 `dcb_async_complete`、`dcb_async_fail` 或 `dcb_async_cancel` 后该 `op_id` 即失效，后续再对该 ID 调用都是 no-op。
:::

### C++ 协程侧等待

```cpp
#include "dart_cpp_bridge/cbridge_wait.hpp"

// 在协程中（必须绑定 Executor）：
auto data = co_await dcb::async_wait(op_id);  // 挂起，不占线程
// data 为 std::vector<uint8_t>；失败时抛 std::runtime_error
```

### 典型场景：C++ 协程调用外部 C 库的异步 API

假设 Dart 侧调用 `fetchUrl(url)` → C++ 协程收到 `url` 参数 → 调用外部 C 库的异步 HTTP API → 库完成后返回响应体 → 协程将结果返回 Dart。

```cpp
#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/cbridge_wait.hpp"
#include "dart_cpp_bridge/dcb_codec.h"   // 纯 C codec（回调用）
#include "dart_cpp_bridge/codec.hpp"     // C++ ByteReader（协程用）
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

// ─── 模拟外部 C 库（代表任何第三方异步库，如 libcurl、libuv 等） ───
// 回调类型：status=0 成功，body/body_len 为响应数据
typedef void (*http_callback)(void* ctx, int status,
                              const uint8_t* body, uint32_t body_len);

// 外部库的异步接口：在自己的线程上完成工作后调用 callback
static void http_client_get_async(const char* url, http_callback cb, void* ctx) {
    // 模拟：库内部开一个线程处理请求（实际库可能是线程池/事件循环）
    std::thread([url = std::string(url), cb, ctx] {
        // 模拟网络延迟...
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 模拟响应体
        std::string response = "response from " + url;
        cb(ctx, 0, (const uint8_t*)response.data(), (uint32_t)response.size());
    }).detach();
}

// ─── 我们的回调（在库的线程上触发） ───
// ctx 是我们传进去的 op_id
static void http_done(void* ctx, int status, const uint8_t* body, uint32_t body_len) {
    uint64_t op_id = (uint64_t)(uintptr_t)ctx;
    if (status == 0) {
        // 用 dcb_codec 编码为 wire 格式（string: u32 len + bytes）
        dcb_writer w;
        dcb_writer_init(&w);
        dcb_write_len_bytes(&w, body, body_len);  // 把原始字节包装为 string
        dcb_async_complete(op_id, w.data, w.len);  // 传给协程
        dcb_writer_free(&w);
    } else {
        dcb_async_fail(op_id, "http request failed");
    }
}

// ─── bridge 协程（由 wire dispatch 调用，参数已从 Dart 解码好） ───
// url 是 wire dispatch 从 Dart 请求中解码出的 std::string
async_simple::coro::Lazy<std::string> fetch_url(std::string url) {
    // 1. 创建异步操作
    uint64_t op_id = dcb_async_create();

    // 2. 启动外部 C 库的异步操作（非阻塞）
    //    传入 url 和回调，库完成后会调用 http_done
    http_client_get_async(url.c_str(), http_done,
                          (void*)(uintptr_t)op_id);

    // 3. 协程挂起，不占 io 线程。库完成后自动恢复。
    auto payload = co_await dcb::async_wait(op_id);
    // payload 是 http_done 中 dcb_async_complete 传入的字节

    // 4. 解码结果（C++ 协程内直接用 ByteReader）
    dcb::ByteReader r(payload.data(), payload.size());
    co_return r.str();  // 对应 http_done 中的 dcb_write_len_bytes
}
```

数据流转：

```text
Dart                     C++ 协程                    外部 C 库
──────────────────────────────────────────────────────────────
fetchUrl("https://...")
    │  wire payload       
    ▼                    
wire dispatch 解码 → url = "https://..."
                         │
                         dcb_async_create() → op_id
                         http_client_get_async(url, cb, op_id)
                         │                          │
                         co_await async_wait(op_id)  │  (库在自己的线程上工作)
                         │  协程挂起 ─────────────  │
                         │                          http_done(op_id, body)
                         │                            dcb_write_len_bytes(body)
                         │                            dcb_async_complete(op_id, ...)
                         │  ◀───────────────────  协程恢复
                         ByteReader(payload).str() → body
                         co_return body
    ◀─── wire response
Dart 收到响应体
```

### 对比 spawn_blocking

| | `dcb_async_*` | `spawn_blocking` |
|--|---|---|
| 是否占线程 | 否（协程挂起） | 是（占一个 pool 线程） |
| 并发上限 | 无（只消耗一个 op_id） | thread_pool 大小（默认 4） |
| 适用场景 | 外部 C 库有异步 API | 外部 C 库只有同步 API |
| C++ 依赖 | 协程侧需要 `cbridge_wait.hpp` | 需要 `runtime.hpp` |

## API 三：调用 Dart 回调

### 函数签名

```c
typedef void (*dcb_dart_fn_callback)(
    void* userdata,
    int ok,              // 1=成功, 0=失败
    const uint8_t* data, // 成功时的编码返回值（wire payload）
    uint32_t data_len,
    const char* error    // 失败时的错误信息（NUL 结尾）
);

int dcb_invoke_dart_fn(
    uint64_t session_id,   // 目标 session
    uint64_t fn_id,        // Dart 闭包 ID
    const uint8_t* args,   // 编码后的参数（wire payload 格式）
    uint32_t args_len,
    dcb_dart_fn_callback callback,
    void* userdata);
```

### 行为

- **非阻塞**：立即返回 0（成功发起）或 -1（session 无效）
- **回调保证被调用恰好一次**（成功或失败）
- **回调在 bridge 的 io 线程上触发**——不要在回调中阻塞
- 可从**任意线程**调用（包括外部运行时的 loop 线程）

### 示例：C 调用 Dart 函数全流程

完整链路：Dart 调 C++ → C++ 把 ID 传给 C 层 → C 层调 Dart 回调 → Dart 返回数据 → C 层处理并唤醒 C++ → C++ 可选返回结果。

**① Dart 侧**——用户写一个普通异步函数 + 调用桥接 API：

```dart
// 用户写的回调：接收一个名字，返回问候语
Future<String> greet(String name) async {
  if (name.isEmpty) throw Exception('name cannot be empty');
  return 'Hello, $name!';
}

// 调用桥接函数（codegen 生成的顶层函数）
// 把回调 + 参数一起传给 C++
final result = await startGreetTask(callback: greet, name: 'World');
print(result); // "Dart said: Hello, World!"
```

**② C++ 侧**——接收 DartFn + 参数，创建异步操作，把一切传给 C 层：

```cpp
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/cbridge_wait.hpp"

// 纯 C 层的前向声明
void c_start_greet(uint64_t session_id, uint64_t fn_id,
                   uint64_t op_id, const char* name);

// wire dispatch 解码后调用此协程
async_simple::coro::Lazy<std::string> start_greet_task(
    dcb::DartFn<std::string(std::string)> callback,
    std::string name) {
  // 1. 提取纯 C API 需要的 ID
  uint64_t session_id = dcb::SessionRegistry::instance().find_id(callback.session());
  uint64_t fn_id = callback.fn_id();

  // 2. 创建异步操作（C 层完成后用来唤醒本协程）
  uint64_t op_id = dcb_async_create();

  // 3. 把 ID + 参数传给 C 层，C 层在某个时刻会回调 Dart
  c_start_greet(session_id, fn_id, op_id, name.c_str());

  // 4. 挂起，等 C 层完成（不占 io 线程）
  auto payload = co_await dcb::async_wait(op_id);

  // 5. 解码 C 层返回的最终结果（可选）
  dcb::ByteReader r(payload.data(), payload.size());
  co_return r.str();
}
```

**③ 纯 C 侧**——在某个时刻调 Dart 回调，拿到结果后唤醒 C++：

```c
#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/dcb_codec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 上下文：C 层需要记住 op_id（用来唤醒 C++）
typedef struct {
    uint64_t op_id;
} greet_ctx;

// Dart 执行完回调后，bridge 在 io 线程触发此函数
// 参数说明：
//   userdata  — 调用 dcb_invoke_dart_fn 时传入的用户指针（原样传回）
//   ok        — 1=Dart 正常返回, 0=Dart 抛了异常
//   data/len  — 成功时：Dart 返回值的编码字节；失败时：NULL/0
//   error     — 失败时：Dart 闭包抛出的异常 message（NUL 结尾）；成功时：NULL
//               例如 Dart 侧 throw Exception('name cannot be empty')
//               → error = "Exception: name cannot be empty"
static void on_dart_reply(void* userdata, int ok, const uint8_t* data,
                          uint32_t len, const char* error) {
    greet_ctx* ctx = (greet_ctx*)userdata;

    if (ok) {
        // 解码 Dart 返回的字符串
        dcb_reader r;
        dcb_reader_init(&r, data, len);
        uint32_t slen;
        const char* greeting = dcb_read_str(&r, &slen);

        // C 层拿到数据后做自己的操作（拼接最终结果）
        char result[256];
        snprintf(result, sizeof(result), "Dart said: %.*s", (int)slen, greeting);

        // 编码结果，唤醒 C++ 协程
        dcb_writer w;
        dcb_writer_init(&w);
        dcb_write_str(&w, result);
        dcb_async_complete(ctx->op_id, w.data, w.len);
        dcb_writer_free(&w);
    } else {
        // Dart 闭包抛了异常（如 throw Exception('...')）
        // bridge 捕获后把 exception.toString() 作为 error 传到这里
        // → 转发给 C++ 协程，co_await 处会抛出 std::runtime_error(error)
        dcb_async_fail(ctx->op_id, error);
    }

    free(ctx);
}

// C++ 调用此函数启动整个流程
void c_start_greet(uint64_t session_id, uint64_t fn_id,
                   uint64_t op_id, const char* name) {
    // 编码要传给 Dart 回调的参数
    dcb_writer w;
    dcb_writer_init(&w);
    dcb_write_str(&w, name);  // Dart 侧收到 name 参数

    // 保存上下文（回调时需要 op_id）
    greet_ctx* ctx = (greet_ctx*)malloc(sizeof(greet_ctx));
    ctx->op_id = op_id;

    // 发起调用（可以在任意线程、任意时刻）
    int rc = dcb_invoke_dart_fn(session_id, fn_id,
                                w.data, w.len,
                                on_dart_reply, ctx);
    dcb_writer_free(&w);

    if (rc != 0) {
        dcb_async_fail(op_id, "invoke failed: invalid session");
        free(ctx);
    }
}
```

数据流转：

```text
Dart                    C++ 协程                    纯 C 层
──────────────────────────────────────────────────────────────
startGreetTask(
  callback: greet,
  name: "World")
    │ wire 传递
    ▼
                    start_greet_task(callback, "World")
                      find_id() → session_id
                      fn_id()
                      dcb_async_create() → op_id
                      │
                      c_start_greet(sid, fn_id, op_id, "World")
                                              │
                                              dcb_write_str("World")
                                              dcb_invoke_dart_fn(...)
    ◀────────────────────────────────────  调用 greet("World")
    greet 执行 → "Hello, World!"
    ────────────────────────────────────▶  on_dart_reply 收到结果
                                              │
                                              拼接 "Dart said: Hello, World!"
                                              dcb_async_complete(op_id, ...)
                                              │
                    co_await 恢复 ◀───────────┘
                    ByteReader.str()
                    co_return "Dart said: Hello, World!"
    ◀─── 最终结果返回 Dart
print(result)
```

## 内部实现原理

```text
dcb_async_create()
  → 创建 oneshot channel，Sender + Receiver 存入全局 registry
  → 返回 op_id

C++ 协程: co_await dcb::async_wait(op_id)
  → 从 registry 取出 Receiver
  → co_await rx.recv() 挂起协程

dcb_async_complete(op_id, data, len)    [任意线程]
  → 从 registry 取出 Sender
  → tx.send(OpResult{ok=true, data})
  → wake_waiter → 协程在其 Executor 上恢复

dcb_async_fail / dcb_async_cancel 类似。
```

`dcb_invoke_dart_fn` 内部：

```text
dcb_invoke_dart_fn(session_id, fn_id, args, len, callback, userdata)
  → 查找 Session
  → spawn_on_asio: 在 io 线程启动协程
    → co_await session->invoke_dart_fn_async(gen, fn_id, args)
      → 发送 DartFnCall 帧到 Dart
      → 挂起等待 Dart 回复
    → Dart 回复 → 协程恢复
    → callback(userdata, ok, data, len, error)
```

## 线程安全

| 函数 | 线程安全 | 说明 |
|------|----------|------|
| `dcb_invoke_dart_fn` | 是 | 可从任意线程调用 |
| `dcb_async_create` | 是 | 全局 mutex 保护 |
| `dcb_async_complete/fail/cancel` | 是 | 可从任意线程调用 |
| `dcb::async_wait` | — | 仅在协程内使用 |

## 设计约束

:::caution
- `dcb_invoke_dart_fn` 的 callback 在 **bridge io 线程**上触发，不要在其中阻塞
- 如需将结果 marshaling 到其他线程，在 callback 中 post 到你的事件循环
- `dcb_async_complete/fail/cancel` 对同一 op_id 只有第一次调用有效，后续为 no-op
- `dcb_async_cancel` 后协程收到 "operation cancelled" 错误
:::

## 完整示例

- **测试用例**: `examples/foreign_runtime_demo` 中的 `test_cbridge_async` / `test_cbridge_invoke`
- **编解码**: `dart/native/include/dart_cpp_bridge/dcb_codec.h`
- **头文件**: `dart/native/include/dart_cpp_bridge/cbridge.h`
- **C++ helper**: `dart/native/include/dart_cpp_bridge/cbridge_wait.hpp`
