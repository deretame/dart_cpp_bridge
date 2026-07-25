#pragma once

// Foreign Runtime C API — 让非 asio 事件循环接入 bridge 的 channel/coroutine 系统。
//
// 外部运行时（libuv、glib、自定义 loop 等）通过注册一个 schedule 回调，
// 即可接收 bridge 投递的任务（如协程恢复），实现跨运行时非阻塞通信。
//
// 详见 docs/foreign_runtime_design.md

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef DART_CPP_BRIDGE_BUILD
#    define DCB_API __declspec(dllexport)
#  else
#    define DCB_API __declspec(dllimport)
#  endif
#else
#  define DCB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// 调度回调类型。
/// bridge 通过此回调向外部运行时投递一个任务。
/// 实现者必须保证 fn(userdata) 在目标事件循环的线程上被调用。
///
/// 参数：
///   fn       - 要执行的函数（通常是协程恢复 trampoline）
///   userdata - fn 的参数（heap-allocated，fn 内部会释放）
///   ctx      - 注册时传入的上下文指针
typedef void (*dcb_schedule_fn)(void (*fn)(void*), void* userdata, void* ctx);

/// 注册一个外部运行时。
///
/// 参数：
///   name        - 运行时名称（调试用，如 "libuv-worker"）
///   schedule_fn - bridge 用来向该运行时投递任务的回调
///   ctx         - 传给 schedule_fn 的上下文（如 UvWorker* 或 uv_loop_t*）
///
/// 返回：runtime_id (>0)，用于后续获取 executor 或注销。
///        返回 0 表示失败。
DCB_API uint32_t dcb_foreign_register(const char* name, dcb_schedule_fn schedule_fn, void* ctx);

/// 注销外部运行时。
/// 注销后，任何对该运行时的 schedule 调用变为 no-op（安全降级）。
/// 已挂起的协程不会被恢复（调用方应确保 channel 已关闭或不再等待）。
DCB_API void dcb_foreign_unregister(uint32_t runtime_id);

/// 从外部运行时向 bridge 主 io_context 投递一个任务。
/// 线程安全，非阻塞。任务在 bridge 的 io 线程上执行。
///
/// 典型用途：外部运行时处理完请求后，通过此函数将结果发回 bridge。
DCB_API void dcb_post_to_bridge(void (*fn)(void*), void* userdata);

/// 获取外部运行时对应的 ForeignExecutor 指针（实际类型为 dcb::ForeignExecutor*）。
/// 返回的指针可直接用于：
///   - channel 的 coAwait(executor) 路径
///   - Lazy.via(executor) 绑定
///   - 任何需要 async_simple::Executor* 的地方
///
/// 指针生命周期与 runtime_id 绑定，unregister 后不可再使用。
/// 返回 NULL 表示 runtime_id 无效。
DCB_API void* dcb_foreign_executor(uint32_t runtime_id);

#ifdef __cplusplus
}
#endif
