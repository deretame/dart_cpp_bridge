#pragma once

// dcb_codec.h — 纯 C wire payload 编解码。
//
// 与 C++ 侧 ByteWriter / ByteReader (codec.hpp) 二进制兼容。
// 纯 C99，不引入任何 C++ 头文件。
//
// 支持类型：i32, u32, i64, u64, f64, bytes/str（长度前缀）、数组（u32 count + N 元素）。
// 所有多字节整数均为小端序。
//
// 用法：
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

/// 动态增长的写入缓冲区。初始分配 64 字节，按需倍增。
typedef struct {
  uint8_t* data;   ///< 内部缓冲区（dcb_writer_free 释放）
  uint32_t len;    ///< 已写入字节数
  uint32_t cap;    ///< 当前容量
} dcb_writer;

/// 初始化 writer（分配初始缓冲区）。
DCB_API void dcb_writer_init(dcb_writer* w);

/// 释放 writer 内部缓冲区。释放后不可再使用（除非重新 init）。
DCB_API void dcb_writer_free(dcb_writer* w);

/// 写入 uint32（4 bytes LE）。
DCB_API void dcb_write_u32(dcb_writer* w, uint32_t v);

/// 写入 uint64（8 bytes LE）。
DCB_API void dcb_write_u64(dcb_writer* w, uint64_t v);

/// 写入 int32（4 bytes LE）。
DCB_API void dcb_write_i32(dcb_writer* w, int32_t v);

/// 写入 int64（8 bytes LE）。
DCB_API void dcb_write_i64(dcb_writer* w, int64_t v);

/// 写入 double（8 bytes LE, IEEE 754）。
DCB_API void dcb_write_f64(dcb_writer* w, double v);

/// 写入长度前缀 + 字节（u32 len + data）。
DCB_API void dcb_write_len_bytes(dcb_writer* w, const void* p, uint32_t n);

/// 写入字符串（u32 strlen + UTF-8 bytes）。便捷包装。
static inline void dcb_write_str(dcb_writer* w, const char* s) {
  dcb_write_len_bytes(w, s, (uint32_t)strlen(s));
}

/// 写入数组头（u32 count）。之后循环写入每个元素即可。
/// 示例：
///   dcb_write_arr_begin(&w, 3);
///   dcb_write_i32(&w, 10);
///   dcb_write_i32(&w, 20);
///   dcb_write_i32(&w, 30);
static inline void dcb_write_arr_begin(dcb_writer* w, uint32_t count) {
  dcb_write_u32(w, count);
}

// ─── Reader ─────────────────────────────────────────────────────────────────

/// 零拷贝读取器。不持有数据所有权，不 malloc。
/// 越界读取时进入错误状态（后续读取返回 0），通过 dcb_reader_valid 检查。
typedef struct {
  const uint8_t* data;  ///< 指向外部缓冲区（调用者持有）
  uint32_t len;         ///< 缓冲区总长度
  uint32_t pos;         ///< 当前读取位置
  int      error;       ///< 越界标志（0=正常, 1=已越界）
} dcb_reader;

/// 初始化 reader，指向外部数据（不拷贝）。
DCB_API void dcb_reader_init(dcb_reader* r, const uint8_t* data, uint32_t len);

/// 返回 1 表示目前无错误，0 表示曾发生越界读取。
DCB_API int dcb_reader_valid(const dcb_reader* r);

/// 读取 uint32（4 bytes LE）。越界时返回 0 并设置 error。
DCB_API uint32_t dcb_read_u32(dcb_reader* r);

/// 读取 uint64（8 bytes LE）。
DCB_API uint64_t dcb_read_u64(dcb_reader* r);

/// 读取 int32（4 bytes LE）。
DCB_API int32_t dcb_read_i32(dcb_reader* r);

/// 读取 int64（8 bytes LE）。
DCB_API int64_t dcb_read_i64(dcb_reader* r);

/// 读取 double（8 bytes LE, IEEE 754）。
DCB_API double dcb_read_f64(dcb_reader* r);

/// 读取长度前缀字节块（u32 len + data）。
/// 返回指向 data 内部的指针（零拷贝），*out_len 为长度。
/// 越界或长度不合法时返回 NULL，*out_len = 0。
DCB_API const uint8_t* dcb_read_len_bytes(dcb_reader* r, uint32_t* out_len);

/// 读取字符串（u32 len + UTF-8 bytes）。
/// 返回指向内部的 const char*（零拷贝，不保证 NUL 结尾），*out_len 为字节长度。
static inline const char* dcb_read_str(dcb_reader* r, uint32_t* out_len) {
  return (const char*)dcb_read_len_bytes(r, out_len);
}

/// 读取数组头，返回元素个数。之后循环读取每个元素即可。
/// 示例：
///   uint32_t n = dcb_read_arr_begin(&r);
///   for (uint32_t i = 0; i < n; i++) vals[i] = dcb_read_i32(&r);
static inline uint32_t dcb_read_arr_begin(dcb_reader* r) {
  return dcb_read_u32(r);
}

#ifdef __cplusplus
}
#endif
