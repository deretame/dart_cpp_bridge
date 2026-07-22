#pragma once

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

// Initialize Dart API DL (pass NativeApi.initializeApiDLData).
// Owning isolate must call this before async replies can be posted.
DCB_API intptr_t dcb_init_dart_api(void* initialize_api_dl_data);

// Start runtime + bind the single process-wide session reply port.
// Only the owning isolate should call this (async replies go here).
DCB_API void dcb_init(int64_t reply_native_port);

// Dispose session (generation++); runtime keeps running.
DCB_API void dcb_dispose(void);

// Dispose session + stop runtime threads.
DCB_API void dcb_shutdown(void);

// Sync RPC (any isolate in-process may call after runtime is up).
DCB_API uint8_t* dcb_invoke_sync(const uint8_t* req, size_t req_len, size_t* out_len,
                                 char** error_out);

// Async / stream: replies on the single session reply port (owning isolate).
DCB_API void dcb_invoke_async(const uint8_t* req, size_t req_len);

DCB_API void dcb_stream_close(uint64_t stream_id);

DCB_API void dcb_free(void* p);

#ifdef __cplusplus
}
#endif
