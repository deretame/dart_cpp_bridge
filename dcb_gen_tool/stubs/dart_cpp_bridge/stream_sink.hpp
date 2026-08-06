// Stub for codegen parsing only — the real header is
// dart/native/include/dart_cpp_bridge/stream_sink.hpp. Codegen only needs
// the dcb::StreamSink template name.
#pragma once

#include <string>

namespace dcb {

template <class T>
class StreamSink {
 public:
  void add(const T&);
  void end();
  void error(const std::string&);
};

}  // namespace dcb
