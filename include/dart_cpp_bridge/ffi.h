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

// Initialize Dart API DL (pass NativeApi.initializeApiDLData from Dart).
DCB_API intptr_t dcb_init_dart_api(void* initialize_api_dl_data);

// Start runtime + bind long-lived reply port (Dart SendPort.nativePort).
DCB_API void dcb_init(int64_t reply_native_port);

// Dispose session (generation++) and stop accepting posts for old gens.
DCB_API void dcb_dispose(void);

// Full shutdown of runtime threads (optional; usually process exit).
DCB_API void dcb_shutdown(void);

// Sync RPC: returns malloc'ed frame buffer; caller must dcb_free.
// On failure returns NULL and sets error_out (malloc'ed message) if non-NULL.
DCB_API uint8_t* dcb_invoke_sync(const uint8_t* req, size_t req_len, size_t* out_len,
                                 char** error_out);

// Async / stream: request is fire-and-forget; replies arrive on reply port.
DCB_API void dcb_invoke_async(const uint8_t* req, size_t req_len);

// Dart closed a stream subscription — further sink.add becomes no-op.
DCB_API void dcb_stream_close(uint64_t stream_id);

DCB_API void dcb_free(void* p);

#ifdef __cplusplus
}
#endif
