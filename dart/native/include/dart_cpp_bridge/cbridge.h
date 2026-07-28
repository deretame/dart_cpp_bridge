#pragma once

// cbridge.h — 纯 C 跨运行时桥接 API。
//
// 为不依赖 async-simple / asio 的 C/C++ 代码提供调用 Dart 回调和
// 异步等待外部操作的能力。任何事件循环、任何线程都可以使用这些 API。
//
// 两类功能：
//   1. dcb_invoke_dart_fn — 从任意 C/C++ 代码调用已注册的 Dart 回调
//   2. dcb_async_*       — 让 C++ 协程非阻塞等待外部 C 异步操作完成
//
// 不引入任何 C++ 头文件。纯 C99 兼容。

#include "dart_cpp_bridge/ffi.h"  // DCB_API macro

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── DartFn 调用（C 回调风格）─────────────────────────────────────────────

/// Dart 回调完成后的通知函数。
/// 在 bridge 的 io 线程上调用（不要在此回调中阻塞）。
/// 如需 marshaling 到其他线程，使用 dcb_async_* 或自行 post。
///
/// @param userdata   调用 dcb_invoke_dart_fn 时传入的用户指针
/// @param ok         1=成功, 0=失败
/// @param data       成功时的编码返回值（wire payload），失败时为 NULL
/// @param data_len   data 的长度
/// @param error      失败时的错误信息（NUL 结尾），成功时为 NULL
typedef void (*dcb_dart_fn_callback)(
    void* userdata,
    int ok,
    const uint8_t* data,
    uint32_t data_len,
    const char* error);

/// 从任意线程调用已注册的 Dart 回调函数。非阻塞。
///
/// @param session_id  目标 session（dcb_session_open 返回的 ID）
/// @param fn_id       Dart 闭包 ID（由 codegen 或手动注册分配）
/// @param args        编码后的参数（wire payload 格式），可为 NULL
/// @param args_len    args 的长度
/// @param callback    Dart 执行完毕后的回调函数
/// @param userdata    透传给 callback 的用户指针
///
/// 返回 0 表示成功发起调用，-1 表示 session 无效。
/// 回调保证被调用恰好一次（成功或失败）。
DCB_API int dcb_invoke_dart_fn(
    uint64_t session_id,
    uint64_t fn_id,
    const uint8_t* args,
    uint32_t args_len,
    dcb_dart_fn_callback callback,
    void* userdata);

// ─── 异步操作原语（C 端完成，C++ 协程端等待）──────────────────────────────

/// 创建一个异步操作，返回操作 ID。
/// C++ 侧可用 dcb::async_wait(id) 在协程中非阻塞等待。
/// C 侧在操作完成时调用 dcb_async_complete / dcb_async_fail。
///
/// 典型用法：C++ 协程调用外部 C 库的异步 API，将 op_id 作为 context 传入，
/// 外部库完成时调用 dcb_async_complete，协程自动恢复。
DCB_API uint64_t dcb_async_create(void);

/// 完成一个异步操作（成功）。可从任意线程调用。
/// data/len 为结果数据（会被拷贝）。调用后 op_id 失效。
DCB_API void dcb_async_complete(uint64_t op_id, const uint8_t* data, uint32_t len);

/// 完成一个异步操作（失败）。可从任意线程调用。
/// error 为 NUL 结尾的错误描述。调用后 op_id 失效。
DCB_API void dcb_async_fail(uint64_t op_id, const char* error);

/// 取消/释放一个未完成的异步操作。
/// 如果 C++ 侧正在等待，协程会收到 "operation cancelled" 错误。
/// 如果无人等待，仅释放资源。安全地对无效 ID 调用（no-op）。
DCB_API void dcb_async_cancel(uint64_t op_id);

#ifdef __cplusplus
}
#endif
