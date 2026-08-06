// Stub for codegen parsing only — the real header is
// dart/native/include/dart_cpp_bridge/codec.hpp. Codegen only needs the
// type names; wire encoding/decoding is generated, not parsed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dcb {

struct Int128 {
  std::int64_t lo;
  std::int64_t hi;
};

struct UInt128 {
  std::uint64_t lo;
  std::uint64_t hi;
};

class ByteReader {
 public:
  std::int32_t i32();
  std::uint32_t u32();
  std::int64_t i64();
  bool u8();
  std::string str();
  template <class T>
  std::vector<T> vec();
};

class ByteWriter {
 public:
  void i32(std::int32_t);
  void u32(std::uint32_t);
  void i64(std::int64_t);
  void u8(std::uint8_t);
  void str(const std::string&);
  template <class T>
  void vec(const std::vector<T>&);
  const std::vector<std::uint8_t>& raw() const;
};

}  // namespace dcb
