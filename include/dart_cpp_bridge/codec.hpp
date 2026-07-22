#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <array>

namespace dcb {

// Wire frame (little-endian):
// magic u32 'DCB1' | version u16 | msg_type u8 | flags u8 |
// request_id u64 | method_id u32 | payload_len u32 | payload...

inline constexpr std::uint32_t kMagic = 0x31424344;  // 'DCB1' LE
inline constexpr std::uint16_t kProtocolVersion = 1;

enum class MsgType : std::uint8_t {
  kRequest = 1,
  kResponseOk = 2,
  kResponseErr = 3,
  kStreamData = 4,
  kStreamEnd = 5,
  kStreamErr = 6,
  // C++ → Dart: invoke a callback registered for this session (FRB DartFn style).
  // frame.request_id = dartfn_reply_id; payload = fn_id u64 + args...
  kDartFnCall = 7,
};

enum class MethodId : std::uint32_t {
  kBridgeVersion = 1,
  kAdd = 2,
  kSleepTest = 3,
  kTicks = 4,
  kEcho = 5,
  kFailAsync = 6,
  kFailStream = 7,
  // payload: fn_id u64 — DartFn async wait on io (co_await)
  kCallDartHello = 8,
  // payload: fn_id u64 — DartFn sync block current thread (no offload babysitting)
  kCallDartHelloSync = 9,
  // payload: opt_i32 — async optional test
  kMaybeDouble = 10,
  // payload: list_i32 — async vector test
  kSumVec = 11,
  // payload: u8vec — async typed list test
  kReverseBytes = 12,
  // payload: i32 enum — async enum test
  kNextStatus = 13,
  // payload: 4 × i32 — async fixed array test
  kSumFixedFour = 14,
  // payload: string + i32 — async struct test
  kGreet = 15,
};

class ByteWriter {
 public:
  void u8(std::uint8_t v) { buf_.push_back(v); }
  void u16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>(v));
    buf_.push_back(static_cast<std::uint8_t>(v >> 8));
  }
  void u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
  }
  void u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
  }
  void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
  void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }

  template <typename T, typename WriteValue>
  void opt(const std::optional<T>& v, WriteValue write_value) {
    if (v.has_value()) {
      u8(1);
      write_value(v.value());
    } else {
      u8(0);
    }
  }

  template <typename E>
  void enume(E v) {
    i32(static_cast<std::int32_t>(v));
  }

  template <typename T, std::size_t N, typename WriteValue>
  void arr(const std::array<T, N>& v, WriteValue write_value) {
    for (const auto& item : v) {
      write_value(item);
    }
  }

  template <typename T, typename WriteValue>
  void vec(const std::vector<T>& v, WriteValue write_value) {
    u32(static_cast<std::uint32_t>(v.size()));
    for (const auto& item : v) {
      write_value(item);
    }
  }

  void u8vec(const std::vector<std::uint8_t>& v) {
    u32(static_cast<std::uint32_t>(v.size()));
    bytes(v.data(), v.size());
  }

  void bytes(const std::uint8_t* data, std::size_t n) {
    buf_.insert(buf_.end(), data, data + n);
  }
  void str(const std::string& s) {
    u32(static_cast<std::uint32_t>(s.size()));
    bytes(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
  }

  std::vector<std::uint8_t>& raw() { return buf_; }
  const std::vector<std::uint8_t>& raw() const { return buf_; }

 private:
  std::vector<std::uint8_t> buf_;
};

class ByteReader {
 public:
  explicit ByteReader(const std::uint8_t* data, std::size_t len)
      : data_(data), len_(len), pos_(0) {}

  std::uint8_t u8() {
    need(1);
    return data_[pos_++];
  }
  std::uint16_t u16() {
    need(2);
    std::uint16_t v = static_cast<std::uint16_t>(data_[pos_]) |
                      (static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }
  std::uint32_t u32() {
    need(4);
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i);
    }
    pos_ += 4;
    return v;
  }
  std::uint64_t u64() {
    need(8);
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
    }
    pos_ += 8;
    return v;
  }
  std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
  std::int64_t i64() { return static_cast<std::int64_t>(u64()); }

  template <typename T, typename ReadValue>
  std::optional<T> opt(ReadValue read_value) {
    const bool has_value = u8() != 0;
    if (!has_value) return std::nullopt;
    return read_value();
  }

  template <typename E>
  E enume() {
    return static_cast<E>(i32());
  }

  template <typename T, std::size_t N, typename ReadValue>
  std::array<T, N> arr(ReadValue read_value) {
    std::array<T, N> result{};
    for (std::size_t i = 0; i < N; ++i) {
      result[i] = read_value();
    }
    return result;
  }

  template <typename T, typename ReadValue>
  std::vector<T> vec(ReadValue read_value) {
    auto n = u32();
    std::vector<T> result;
    result.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
      result.push_back(read_value());
    }
    return result;
  }

  std::vector<std::uint8_t> u8vec() {
    auto n = u32();
    need(n);
    std::vector<std::uint8_t> result(data_ + pos_, data_ + pos_ + n);
    pos_ += n;
    return result;
  }

  std::string str() {
    auto n = u32();
    need(n);
    std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
    pos_ += n;
    return s;
  }

  const std::uint8_t* remaining(std::size_t* out_n) const {
    *out_n = len_ - pos_;
    return data_ + pos_;
  }

 private:
  void need(std::size_t n) {
    if (pos_ + n > len_) {
      throw std::runtime_error("codec: truncated buffer");
    }
  }

  const std::uint8_t* data_;
  std::size_t len_;
  std::size_t pos_;
};

inline std::vector<std::uint8_t> make_frame(MsgType type, std::uint64_t request_id,
                                            std::uint32_t method_id,
                                            const std::vector<std::uint8_t>& payload) {
  ByteWriter w;
  w.u32(kMagic);
  w.u16(kProtocolVersion);
  w.u8(static_cast<std::uint8_t>(type));
  w.u8(0);
  w.u64(request_id);
  w.u32(method_id);
  w.u32(static_cast<std::uint32_t>(payload.size()));
  if (!payload.empty()) {
    w.bytes(payload.data(), payload.size());
  }
  return std::move(w.raw());
}

struct FrameHeader {
  MsgType type{};
  std::uint8_t flags{};
  std::uint64_t request_id{};
  std::uint32_t method_id{};
  std::vector<std::uint8_t> payload;
};

inline FrameHeader parse_frame(const std::uint8_t* data, std::size_t len) {
  ByteReader r(data, len);
  if (r.u32() != kMagic) {
    throw std::runtime_error("codec: bad magic");
  }
  if (r.u16() != kProtocolVersion) {
    throw std::runtime_error("codec: bad version");
  }
  FrameHeader h;
  h.type = static_cast<MsgType>(r.u8());
  h.flags = r.u8();
  h.request_id = r.u64();
  h.method_id = r.u32();
  auto plen = r.u32();
  std::size_t rem = 0;
  auto* p = r.remaining(&rem);
  if (rem < plen) {
    throw std::runtime_error("codec: payload truncated");
  }
  h.payload.assign(p, p + plen);
  return h;
}

}  // namespace dcb
