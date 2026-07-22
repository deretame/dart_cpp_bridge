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

// Per-isolate: pass NativeApi.initializeApiDLData.
DCB_API intptr_t dcb_init_dart_api(void* initialize_api_dl_data);

// Open a session for this isolate's reply port. Starts shared runtime if needed.
// Returns session_id (>0), or 0 on failure.
DCB_API uint64_t dcb_session_open(int64_t reply_native_port);

// Close one session (does not stop runtime).
DCB_API void dcb_session_close(uint64_t session_id);

// NativeFinalizer callback: [token] points to a malloc'd uint64_t session_id.
// Frees [token] after close. Safe if session already closed.
DCB_API void dcb_session_finalizer(void* token);

// Close all sessions + stop runtime.
DCB_API void dcb_shutdown(void);

DCB_API uint8_t* dcb_invoke_sync(uint64_t session_id, const uint8_t* req, size_t req_len,
                                 size_t* out_len, char** error_out);

DCB_API void dcb_invoke_async(uint64_t session_id, const uint8_t* req, size_t req_len);

DCB_API void dcb_stream_close(uint64_t session_id, uint64_t stream_id);

DCB_API void dcb_free(void* p);

#ifdef __cplusplus
}
#endif
