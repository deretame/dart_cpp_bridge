#pragma once

// dcb_codec.h — Pure C wire payload codec.
//
// Binary-compatible with the C++ ByteWriter / ByteReader (codec.hpp).
// Pure C99; no C++ headers are introduced.
//
// Supported types: i32, u32, i64, u64, f64, bytes/str (length-prefixed), array (u32 count + N elements).
// All multi-byte integers are little-endian.
//
// Usage:
//   dcb_writer w;
//   dcb_writer_init(&w);
//   dcb_write_str(&w, "hello");
//   dcb_write_i32(&w, 42);
//   dcb_invoke_dart_fn(sid, fn_id, w.data, w.len, cb, ud);
//   dcb_writer_free(&w);
//
//   dcb_reader r;
//   dcb_reader_init(&r, data, data_len);
//   uint32_t slen;
//   const char* s = dcb_read_str(&r, &slen);
//   int32_t v = dcb_read_i32(&r);

#include "dart_cpp_bridge/ffi.h"  // DCB_API macro

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ─── Writer ─────────────────────────────────────────────────────────────────

/// Dynamically growing write buffer. Initially allocates 64 bytes and doubles on demand.
typedef struct {
  uint8_t* data;   ///< Internal buffer (freed by dcb_writer_free)
  uint32_t len;    ///< Number of bytes written
  uint32_t cap;    ///< Current capacity
} dcb_writer;

/// Initialize the writer (allocate the initial buffer).
DCB_API void dcb_writer_init(dcb_writer* w);

/// Free the writer's internal buffer. The writer must not be used after this (unless re-initialized).
DCB_API void dcb_writer_free(dcb_writer* w);

/// Write uint32 (4 bytes LE).
DCB_API void dcb_write_u32(dcb_writer* w, uint32_t v);

/// Write uint64 (8 bytes LE).
DCB_API void dcb_write_u64(dcb_writer* w, uint64_t v);

/// Write int32 (4 bytes LE).
DCB_API void dcb_write_i32(dcb_writer* w, int32_t v);

/// Write int64 (8 bytes LE).
DCB_API void dcb_write_i64(dcb_writer* w, int64_t v);

/// Write double (8 bytes LE, IEEE 754).
DCB_API void dcb_write_f64(dcb_writer* w, double v);

/// Write length-prefixed bytes (u32 len + data).
DCB_API void dcb_write_len_bytes(dcb_writer* w, const void* p, uint32_t n);

/// Write a string (u32 strlen + UTF-8 bytes). Convenience wrapper.
static inline void dcb_write_str(dcb_writer* w, const char* s) {
  dcb_write_len_bytes(w, s, (uint32_t)strlen(s));
}

/// Write an array header (u32 count). Loop to write each element afterwards.
/// Example:
///   dcb_write_arr_begin(&w, 3);
///   dcb_write_i32(&w, 10);
///   dcb_write_i32(&w, 20);
///   dcb_write_i32(&w, 30);
static inline void dcb_write_arr_begin(dcb_writer* w, uint32_t count) {
  dcb_write_u32(w, count);
}

// ─── Reader ─────────────────────────────────────────────────────────────────

/// Zero-copy reader. Does not own the data and does not malloc.
/// On out-of-bounds reads it enters an error state (subsequent reads return 0); check via dcb_reader_valid.
typedef struct {
  const uint8_t* data;  ///< Pointer to external buffer (owned by caller)
  uint32_t len;         ///< Total buffer length
  uint32_t pos;         ///< Current read position
  int      error;       ///< Out-of-bounds flag (0=ok, 1=out-of-bounds)
} dcb_reader;

/// Initialize the reader to point at external data (no copy).
DCB_API void dcb_reader_init(dcb_reader* r, const uint8_t* data, uint32_t len);

/// Return 1 if no error has occurred, 0 if an out-of-bounds read happened.
DCB_API int dcb_reader_valid(const dcb_reader* r);

/// Read uint32 (4 bytes LE). Returns 0 and sets error on out-of-bounds.
DCB_API uint32_t dcb_read_u32(dcb_reader* r);

/// Read uint64 (8 bytes LE).
DCB_API uint64_t dcb_read_u64(dcb_reader* r);

/// Read int32 (4 bytes LE).
DCB_API int32_t dcb_read_i32(dcb_reader* r);

/// Read int64 (8 bytes LE).
DCB_API int64_t dcb_read_i64(dcb_reader* r);

/// Read double (8 bytes LE, IEEE 754).
DCB_API double dcb_read_f64(dcb_reader* r);

/// Read a length-prefixed byte block (u32 len + data).
/// Returns a pointer into the data (zero-copy); *out_len is the length.
/// Returns NULL and sets *out_len = 0 on out-of-bounds or invalid length.
DCB_API const uint8_t* dcb_read_len_bytes(dcb_reader* r, uint32_t* out_len);

/// Read a string (u32 len + UTF-8 bytes).
/// Returns an internal const char* (zero-copy, not guaranteed NUL-terminated); *out_len is the byte length.
static inline const char* dcb_read_str(dcb_reader* r, uint32_t* out_len) {
  return (const char*)dcb_read_len_bytes(r, out_len);
}

/// Read an array header and return the element count. Loop to read each element afterwards.
/// Example:
///   uint32_t n = dcb_read_arr_begin(&r);
///   for (uint32_t i = 0; i < n; i++) vals[i] = dcb_read_i32(&r);
static inline uint32_t dcb_read_arr_begin(dcb_reader* r) {
  return dcb_read_u32(r);
}

#ifdef __cplusplus
}
#endif
