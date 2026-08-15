---
title: Pure C Bridge API
description: C99-compatible API for pure C callers — encoding/decoding, async operations, and calling Dart functions
---

:::caution[v2 development line]
The C ABI in this chapter is stable across v1 and v2. C++ coroutine examples use
`stdexec::task`; pure C callers still need no C++ dependency.
:::

This page introduces the **pure C API** provided by `cbridge.h` and `dcb_codec.h`, plus `cbridge_wait.hpp` used together with C++ coroutines.

You can use it for three things:

- Use `dcb_codec.h` to encode/decode wire data, binary-compatible with the C++ side `ByteWriter` / `ByteReader`
- Use `dcb_invoke_dart_fn` to call registered Dart callbacks from any thread
- Use `dcb_async_*` with `cbridge_wait.hpp` to let C++ coroutines wait non-blocking for external C async operations to complete

If your code is a **pure C project, can only export C symbols, or you don't want to bring C++20 coroutines into the caller**, use the C entry points in this chapter.

> Other language runtimes (such as Python `ctypes`, Rust FFI, Go `cgo`) can also use this C API as the minimal entry point.

## When to Use the Pure C API

Choose this entry point in the following cases:

- **Your code is a pure C project, or can only export C symbols**: C compilation units have no `co_await`, templates, or `Executor`, so they cannot directly use the bridge's C++ coroutine entry points.
- **You call the bridge from another language runtime**: Python `ctypes`, Rust FFI, Go `cgo`, etc. can only bind to the C ABI.
- **You want the caller to keep zero C++ dependencies**: the C side only compiles C headers; the bridge's internal async runtime is invisible to the caller.

:::caution[C side cannot await]
**C code itself cannot `co_await` these async operations.** The awaiting side of `dcb_async_*` is C++ coroutines (via `cbridge_wait.hpp`); the C side is only responsible for creating, completing, or canceling operations.
:::

The pure C API is the **lowest-common-denominator entry with zero C++ dependencies**:

```text
Any C/C++ runtime (regardless of which coroutine/event loop it uses)
    │
    │  dcb_codec.h   — encode/decode parameters/return values
    │  dcb_async_*   — async operation primitives
    │  dcb_invoke_dart_fn — invoke Dart callbacks
    ▼
bridge internals (C++ coroutine pipeline; caller need not know)
    │
    ▼
Dart Isolate executes the callback → result returned via callback
```

## Headers

```c
#include "dart_cpp_bridge/dcb_codec.h"  // wire payload encoding/decoding
#include "dart_cpp_bridge/cbridge.h"    // async operation primitives + invoke Dart callbacks
// Pure C99; does not introduce any C++ header files
```

## API 1: Pure C Codec

`dcb_codec.h` provides a pure C99 codec that is **binary compatible** with the C++ side `ByteWriter` / `ByteReader` (codec.hpp).

### Supported Types

| C type | Wire encoding | Description |
|--------|---------------|-------------|
| `int32_t` | 4 bytes LE | `bool` also uses this (0/1) |
| `uint32_t` | 4 bytes LE | |
| `int64_t` | 8 bytes LE | |
| `uint64_t` | 8 bytes LE | |
| `double` | 8 bytes LE (IEEE 754) | |
| `const char*` | u32 len + UTF-8 bytes | Only variable-length type |
| Array | u32 count + N elements | Elements can be any basic type above |

Array element types supported: `i32`, `u32`, `i64`, `u64`, `f64`, `str`. All elements in the same array must have the same type (caller guarantees this).

struct / pointer / nested container are not supported — serialize complex data as a string (JSON, protobuf, etc.).

### Writer

Dynamically growing buffer, initially 64 bytes, doubling on demand. Internally uses `malloc`/`realloc`; release with `dcb_writer_free` after use.

```c
dcb_writer w;
dcb_writer_init(&w);

dcb_write_str(&w, "hello");     // u32 strlen + UTF-8 bytes
dcb_write_i32(&w, 42);          // 4 bytes LE
dcb_write_f64(&w, 3.14);        // 8 bytes LE
dcb_write_u64(&w, 123456789ULL); // 8 bytes LE

// Array: write count first, then loop to write elements
dcb_write_arr_begin(&w, 3);
dcb_write_i32(&w, 10);
dcb_write_i32(&w, 20);
dcb_write_i32(&w, 30);

// Use w.data / w.len as the payload
dcb_invoke_dart_fn(session_id, fn_id, w.data, w.len, callback, userdata);

dcb_writer_free(&w);  // free internal buffer
```

Full Writer API:

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

Zero-copy reader — no `malloc`, no ownership of data. Out-of-bounds reads put it in an error state (subsequent reads return 0); check with `dcb_reader_valid`.

:::caution[Must read in the same order as written]
Reader is a **sequential cursor** with no random access. If the write order is `str → i32 → f64`, reads must also call in the order `str → i32 → f64`. Skipping or reading out of order causes all subsequent fields to be parsed incorrectly (offset misalignment). Types and order must match the writer side exactly.
:::

```c
// data / data_len come from the callback arguments of dcb_invoke_dart_fn
dcb_reader r;
dcb_reader_init(&r, data, data_len);

uint32_t slen;
const char* s = dcb_read_str(&r, &slen);  // zero-copy, points inside data
int32_t v = dcb_read_i32(&r);
double d = dcb_read_f64(&r);

// Read array
uint32_t n = dcb_read_arr_begin(&r);
for (uint32_t i = 0; i < n; i++) {
    int32_t elem = dcb_read_i32(&r);
    // ...
}

if (!dcb_reader_valid(&r)) {
    // data format mismatch, read out of bounds
}
```

:::note
The pointer returned by `dcb_read_str` is **not guaranteed to be NUL-terminated**; its length is `*out_len`. If you need a C string, copy it and append `'\0'` yourself.
:::

Full Reader API:

```c
void dcb_reader_init(dcb_reader* r, const uint8_t* data, uint32_t len);
int  dcb_reader_valid(const dcb_reader* r);  // 1=ok, 0=out-of-bounds occurred
uint32_t dcb_read_u32(dcb_reader* r);
uint64_t dcb_read_u64(dcb_reader* r);
int32_t  dcb_read_i32(dcb_reader* r);
int64_t  dcb_read_i64(dcb_reader* r);
double   dcb_read_f64(dcb_reader* r);
const uint8_t* dcb_read_len_bytes(dcb_reader* r, uint32_t* out_len);  // zero-copy
const char*    dcb_read_str(dcb_reader* r, uint32_t* out_len);        // inline cast
uint32_t       dcb_read_arr_begin(dcb_reader* r);                     // inline: u32 count
```

### Mapping to the C++ Codec

| Pure C (`dcb_codec.h`) | C++ (`codec.hpp`) | Wire format |
|---|---|---|
| `dcb_write_i32` | `ByteWriter::i32()` | 4 bytes LE |
| `dcb_write_u32` | `ByteWriter::u32()` | 4 bytes LE |
| `dcb_write_i64` | `ByteWriter::i64()` | 8 bytes LE |
| `dcb_write_u64` | `ByteWriter::u64()` | 8 bytes LE |
| `dcb_write_f64` | `ByteWriter::f64()` | 8 bytes LE |
| `dcb_write_str` | `ByteWriter::str()` | u32 len + bytes |
| `dcb_write_arr_begin` + loop | `ByteWriter::vec()` header | u32 count + N elements |
| `dcb_read_str` | `ByteReader::str()` | zero-copy |

Encoding is identical on both sides; data written from the pure C side can be read directly by the C++ side, and vice versa.

## API 2: Async Operation Primitives

These functions are used on the **C side** to create, complete, or cancel an async operation. The C++ coroutine side waits non-blocking via `dcb::async_wait()` in `cbridge_wait.hpp`, without occupying a bridge thread.

### Function Signatures

```c
uint64_t dcb_async_create(void);                                    // create operation
void dcb_async_complete(uint64_t op_id, const uint8_t* data, uint32_t len);  // complete successfully
void dcb_async_fail(uint64_t op_id, const char* error);             // fail
void dcb_async_cancel(uint64_t op_id);                              // cancel
```

:::note[One-shot operation]
The `op_id` returned by `dcb_async_create()` is one-shot. After calling `dcb_async_complete`, `dcb_async_fail`, or `dcb_async_cancel`, that `op_id` becomes invalid; subsequent calls on that ID are no-ops.
:::

### Waiting on the C++ Coroutine Side

```cpp
#include "dart_cpp_bridge/cbridge_wait.hpp"

// Inside a coroutine (must be bound to an Executor):
auto data = co_await dcb::async_wait(op_id);  // suspends, does not occupy a thread
// data is std::vector<uint8_t>; throws std::runtime_error on failure
```

### Typical Scenario: C++ Coroutine Calls an Async API of an External C Library

Suppose the Dart side calls `fetchUrl(url)` → C++ coroutine receives the `url` argument → calls the async HTTP API of an external C library → the library returns the response body when done → the coroutine returns the result to Dart.

```cpp
#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/cbridge_wait.hpp"
#include "dart_cpp_bridge/dcb_codec.h"   // pure C codec (for callbacks)
#include "dart_cpp_bridge/codec.hpp"     // C++ ByteReader (for coroutines)
#include <string>
#include <thread>
#include <chrono>
#include <cstring>

// ─── Simulate an external C library (represents any third-party async library, e.g. libcurl, libuv, etc.) ───
// Callback type: status=0 means success, body/body_len is the response data
typedef void (*http_callback)(void* ctx, int status,
                              const uint8_t* body, uint32_t body_len);

// Async interface of the external library: it calls the callback after doing work on its own thread
static void http_client_get_async(const char* url, http_callback cb, void* ctx) {
    // Simulate: library internally spawns a thread to process the request
    // (real library may use a thread pool / event loop)
    std::thread([url = std::string(url), cb, ctx] {
        // Simulate network latency...
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Simulate response body
        std::string response = "response from " + url;
        cb(ctx, 0, (const uint8_t*)response.data(), (uint32_t)response.size());
    }).detach();
}

// ─── Our callback (triggered on the library's thread) ───
// ctx is the op_id we passed in
static void http_done(void* ctx, int status, const uint8_t* body, uint32_t body_len) {
    uint64_t op_id = (uint64_t)(uintptr_t)ctx;
    if (status == 0) {
        // Use dcb_codec to encode into wire format (string: u32 len + bytes)
        dcb_writer w;
        dcb_writer_init(&w);
        dcb_write_len_bytes(&w, body, body_len);  // wrap raw bytes as a string
        dcb_async_complete(op_id, w.data, w.len);  // pass to coroutine
        dcb_writer_free(&w);
    } else {
        dcb_async_fail(op_id, "http request failed");
    }
}

// ─── bridge coroutine (called by wire dispatch; arguments already decoded from Dart) ───
// url is the std::string decoded from the Dart request by wire dispatch
stdexec::task<std::string> fetch_url(std::string url) {
    // 1. create async operation
    uint64_t op_id = dcb_async_create();

    // 2. start the external C library's async operation (non-blocking)
    //    pass in url and callback; the library will call http_done when done
    http_client_get_async(url.c_str(), http_done,
                          (void*)(uintptr_t)op_id);

    // 3. coroutine suspends, does not occupy the io thread. Automatically resumes when the library completes.
    auto payload = co_await dcb::async_wait(op_id);
    // payload are the bytes passed to dcb_async_complete in http_done

    // 4. decode result (use ByteReader directly inside the C++ coroutine)
    dcb::ByteReader r(payload.data(), payload.size());
    co_return r.str();  // corresponds to dcb_write_len_bytes in http_done
}
```

Data flow:

```text
Dart                     C++ coroutine               External C library
──────────────────────────────────────────────────────────────
fetchUrl("https://...")
    │  wire payload       
    ▼                    
wire dispatch decodes → url = "https://..."
                         │
                         dcb_async_create() → op_id
                         http_client_get_async(url, cb, op_id)
                         │                          │
                         co_await async_wait(op_id)  │  (library works on its own thread)
                         │  coroutine suspends ─────────────  │
                         │                          http_done(op_id, body)
                         │                            dcb_write_len_bytes(body)
                         │                            dcb_async_complete(op_id, ...)
                         │  ◀───────────────────  coroutine resumes
                         ByteReader(payload).str() → body
                         co_return body
    ◀─── wire response
Dart receives response body
```

### Comparison with spawn_blocking

| | `dcb_async_*` | `spawn_blocking` |
|--|---|---|
| Occupies thread? | No (coroutine suspends) | Yes (occupies a pool thread) |
| Concurrency limit | None (only consumes one op_id) | thread_pool size (default 4) |
| Use case | External C library has async API | External C library only has sync API |
| C++ dependency | Coroutine side needs `cbridge_wait.hpp` | Needs `runtime.hpp` |

## API 3: Calling Dart Callbacks

### Function Signatures

```c
typedef void (*dcb_dart_fn_callback)(
    void* userdata,
    int ok,              // 1=success, 0=failure
    const uint8_t* data, // encoded return value on success (wire payload)
    uint32_t data_len,
    const char* error    // error message on failure (NUL-terminated)
);

int dcb_invoke_dart_fn(
    uint64_t session_id,   // target session
    uint64_t fn_id,        // Dart closure ID
    const uint8_t* args,   // encoded arguments (wire payload format)
    uint32_t args_len,
    dcb_dart_fn_callback callback,
    void* userdata);
```

### Behavior

- **Non-blocking**: returns 0 immediately (successfully initiated) or -1 (invalid session)
- **Callback is guaranteed to be called exactly once** (success or failure)
- **Callback is triggered on the bridge's io thread** — don't block in the callback
- Can be called from **any thread** (including external runtime loop threads)

### Example: Full Flow of C Calling a Dart Function

Full chain: Dart calls C++ → C++ passes IDs to the C layer → C layer calls Dart callback → Dart returns data → C layer processes and wakes C++ → C++ optionally returns a result.

**① Dart side** — user writes a normal async function + calls the bridge API:

```dart
// User-written callback: receives a name and returns a greeting
Future<String> greet(String name) async {
  if (name.isEmpty) throw Exception('name cannot be empty');
  return 'Hello, $name!';
}

// Call the bridge function (a codegen-generated top-level function)
// Pass the callback + arguments together to C++
final result = await startGreetTask(callback: greet, name: 'World');
print(result); // "Dart said: Hello, World!"
```

**② C++ side** — receives DartFn + arguments, creates an async operation, passes everything to the C layer:

```cpp
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/cbridge_wait.hpp"

// Forward declaration for the pure C layer
void c_start_greet(uint64_t session_id, uint64_t fn_id,
                   uint64_t op_id, const char* name);

// wire dispatch calls this coroutine after decoding
stdexec::task<std::string> start_greet_task(
    dcb::DartFn<std::string(std::string)> callback,
    std::string name) {
  // 1. extract the IDs needed by the pure C API
  uint64_t session_id = dcb::SessionRegistry::instance().find_id(callback.session());
  uint64_t fn_id = callback.fn_id();

  // 2. create async operation (used to wake this coroutine when the C layer completes)
  uint64_t op_id = dcb_async_create();

  // 3. pass IDs + arguments to the C layer; the C layer will call back Dart at some point
  c_start_greet(session_id, fn_id, op_id, name.c_str());

  // 4. suspend and wait for the C layer to complete (does not occupy the io thread)
  auto payload = co_await dcb::async_wait(op_id);

  // 5. decode the final result returned by the C layer (optional)
  dcb::ByteReader r(payload.data(), payload.size());
  co_return r.str();
}
```

**③ Pure C side** — at some point call the Dart callback, then wake C++ after getting the result:

```c
#include "dart_cpp_bridge/cbridge.h"
#include "dart_cpp_bridge/dcb_codec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Context: the C layer needs to remember op_id (used to wake C++)
typedef struct {
    uint64_t op_id;
} greet_ctx;

// After Dart finishes executing the callback, the bridge triggers this function on the io thread
// Parameter description:
//   userdata  — the user pointer passed when calling dcb_invoke_dart_fn (passed back unchanged)
//   ok        — 1=Dart returned normally, 0=Dart threw an exception
//   data/len  — on success: encoded bytes of Dart's return value; on failure: NULL/0
//   error     — on failure: the message of the exception thrown by the Dart closure (NUL-terminated); on success: NULL
//               For example, on the Dart side throw Exception('name cannot be empty')
//               → error = "Exception: name cannot be empty"
static void on_dart_reply(void* userdata, int ok, const uint8_t* data,
                          uint32_t len, const char* error) {
    greet_ctx* ctx = (greet_ctx*)userdata;

    if (ok) {
        // decode the string returned by Dart
        dcb_reader r;
        dcb_reader_init(&r, data, len);
        uint32_t slen;
        const char* greeting = dcb_read_str(&r, &slen);

        // The C layer does its own processing after getting the data (concatenate final result)
        char result[256];
        snprintf(result, sizeof(result), "Dart said: %.*s", (int)slen, greeting);

        // encode the result and wake the C++ coroutine
        dcb_writer w;
        dcb_writer_init(&w);
        dcb_write_str(&w, result);
        dcb_async_complete(ctx->op_id, w.data, w.len);
        dcb_writer_free(&w);
    } else {
        // The Dart closure threw an exception (e.g. throw Exception('...'))
        // The bridge catches it and passes exception.toString() as error here
        // → forward to the C++ coroutine; the co_await site will throw std::runtime_error(error)
        dcb_async_fail(ctx->op_id, error);
    }

    free(ctx);
}

// C++ calls this function to start the whole flow
void c_start_greet(uint64_t session_id, uint64_t fn_id,
                   uint64_t op_id, const char* name) {
    // encode the arguments to be passed to the Dart callback
    dcb_writer w;
    dcb_writer_init(&w);
    dcb_write_str(&w, name);  // Dart side receives the name argument

    // save context (op_id needed in the callback)
    greet_ctx* ctx = (greet_ctx*)malloc(sizeof(greet_ctx));
    ctx->op_id = op_id;

    // initiate the call (can be done on any thread, at any time)
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

Data flow:

```text
Dart                    C++ coroutine               Pure C layer
──────────────────────────────────────────────────────────────
startGreetTask(
  callback: greet,
  name: "World")
    │ wire transfer
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
    ◀────────────────────────────────────  invoke greet("World")
    greet executes → "Hello, World!"
    ────────────────────────────────────▶  on_dart_reply receives result
                                              │
                                              concatenate "Dart said: Hello, World!"
                                              dcb_async_complete(op_id, ...)
                                              │
                    co_await resumes ◀───────────┘
                    ByteReader.str()
                    co_return "Dart said: Hello, World!"
    ◀─── final result returned to Dart
print(result)
```

## Internal Implementation

```text
dcb_async_create()
  → create oneshot channel, store Sender + Receiver in the global registry
  → return op_id

C++ coroutine: co_await dcb::async_wait(op_id)
  → take Receiver from registry
  → co_await rx.recv() suspends coroutine

dcb_async_complete(op_id, data, len)    [any thread]
  → take Sender from registry
  → tx.send(OpResult{ok=true, data})
  → wake_waiter → coroutine resumes on its Executor

dcb_async_fail / dcb_async_cancel similar.
```

Inside `dcb_invoke_dart_fn`:

```text
dcb_invoke_dart_fn(session_id, fn_id, args, len, callback, userdata)
  → look up Session
  → spawn_on_asio: start coroutine on the io thread
    → co_await session->invoke_dart_fn_async(gen, fn_id, args)
      → send DartFnCall frame to Dart
      → suspend and wait for Dart reply
    → Dart replies → coroutine resumes
    → callback(userdata, ok, data, len, error)
```

## Thread Safety

| Function | Thread-safe | Note |
|------|----------|------|
| `dcb_invoke_dart_fn` | Yes | Can be called from any thread |
| `dcb_async_create` | Yes | Protected by global mutex |
| `dcb_async_complete/fail/cancel` | Yes | Can be called from any thread |
| `dcb::async_wait` | — | Only use inside a coroutine |

## Design Constraints

:::caution
- `dcb_invoke_dart_fn`'s callback is triggered on the **bridge io thread**, don't block in it
- If you need to marshal results to another thread, post to your event loop in the callback
- Only the first call to `dcb_async_complete/fail/cancel` on a given `op_id` is effective; subsequent calls are no-op
- After `dcb_async_cancel`, the coroutine receives an "operation cancelled" error
:::

## Full Examples

- **Test cases**: `examples/foreign_runtime_demo` — `test_cbridge_async` / `test_cbridge_invoke`
- **Codec**: `dart/native/include/dart_cpp_bridge/dcb_codec.h`
- **Header**: `dart/native/include/dart_cpp_bridge/cbridge.h`
- **C++ helper**: `dart/native/include/dart_cpp_bridge/cbridge_wait.hpp`
