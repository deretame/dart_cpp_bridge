// dcb_codec.c — Pure C wire payload codec implementation.
// Binary-compatible with the C++ ByteWriter / ByteReader (codec.hpp).

#include "dart_cpp_bridge/dcb_codec.h"

#include <stdlib.h>
#include <string.h>

// ─── Writer ─────────────────────────────────────────────────────────────────

#define DCB_WRITER_INIT_CAP 64

static void dcb_writer_ensure(dcb_writer* w, uint32_t extra) {
  uint32_t need = w->len + extra;
  if (need <= w->cap) return;
  uint32_t new_cap = w->cap ? w->cap : DCB_WRITER_INIT_CAP;
  while (new_cap < need) new_cap *= 2;
  uint8_t* p = (uint8_t*)realloc(w->data, new_cap);
  if (!p) return;  // OOM: silently fail (write is lost, len is not updated)
  w->data = p;
  w->cap = new_cap;
}

void dcb_writer_init(dcb_writer* w) {
  w->data = (uint8_t*)malloc(DCB_WRITER_INIT_CAP);
  w->len = 0;
  w->cap = w->data ? DCB_WRITER_INIT_CAP : 0;
}

void dcb_writer_free(dcb_writer* w) {
  free(w->data);
  w->data = NULL;
  w->len = 0;
  w->cap = 0;
}

void dcb_write_u32(dcb_writer* w, uint32_t v) {
  dcb_writer_ensure(w, 4);
  if (w->len + 4 > w->cap) return;
  w->data[w->len + 0] = (uint8_t)(v);
  w->data[w->len + 1] = (uint8_t)(v >> 8);
  w->data[w->len + 2] = (uint8_t)(v >> 16);
  w->data[w->len + 3] = (uint8_t)(v >> 24);
  w->len += 4;
}

void dcb_write_u64(dcb_writer* w, uint64_t v) {
  dcb_writer_ensure(w, 8);
  if (w->len + 8 > w->cap) return;
  for (int i = 0; i < 8; ++i) {
    w->data[w->len + i] = (uint8_t)(v >> (8 * i));
  }
  w->len += 8;
}

void dcb_write_i32(dcb_writer* w, int32_t v) {
  dcb_write_u32(w, (uint32_t)v);
}

void dcb_write_i64(dcb_writer* w, int64_t v) {
  dcb_write_u64(w, (uint64_t)v);
}

void dcb_write_f64(dcb_writer* w, double v) {
  uint64_t bits;
  memcpy(&bits, &v, sizeof(bits));
  dcb_write_u64(w, bits);
}

static void dcb_write_raw(dcb_writer* w, const void* p, uint32_t n) {
  if (n == 0) return;
  dcb_writer_ensure(w, n);
  if (w->len + n > w->cap) return;
  memcpy(w->data + w->len, p, n);
  w->len += n;
}

void dcb_write_len_bytes(dcb_writer* w, const void* p, uint32_t n) {
  dcb_write_u32(w, n);
  dcb_write_raw(w, p, n);
}

// ─── Reader ─────────────────────────────────────────────────────────────────

void dcb_reader_init(dcb_reader* r, const uint8_t* data, uint32_t len) {
  r->data = data;
  r->len = len;
  r->pos = 0;
  r->error = 0;
}

int dcb_reader_valid(const dcb_reader* r) {
  return !r->error;
}

uint32_t dcb_read_u32(dcb_reader* r) {
  if (r->error || r->pos + 4 > r->len) {
    r->error = 1;
    return 0;
  }
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= (uint32_t)r->data[r->pos + i] << (8 * i);
  }
  r->pos += 4;
  return v;
}

uint64_t dcb_read_u64(dcb_reader* r) {
  if (r->error || r->pos + 8 > r->len) {
    r->error = 1;
    return 0;
  }
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= (uint64_t)r->data[r->pos + i] << (8 * i);
  }
  r->pos += 8;
  return v;
}

int32_t dcb_read_i32(dcb_reader* r) {
  return (int32_t)dcb_read_u32(r);
}

int64_t dcb_read_i64(dcb_reader* r) {
  return (int64_t)dcb_read_u64(r);
}

double dcb_read_f64(dcb_reader* r) {
  uint64_t bits = dcb_read_u64(r);
  double v;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

const uint8_t* dcb_read_len_bytes(dcb_reader* r, uint32_t* out_len) {
  uint32_t n = dcb_read_u32(r);
  if (r->error || r->pos + n > r->len) {
    r->error = 1;
    if (out_len) *out_len = 0;
    return NULL;
  }
  const uint8_t* p = r->data + r->pos;
  r->pos += n;
  if (out_len) *out_len = n;
  return p;
}
