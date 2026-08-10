// Stub for codegen parsing only — the real header ships with Boost.Asio
// (used when DCB_USE_BOOST_ASIO is defined). Codegen only needs the class name.
#pragma once

namespace boost::asio {
class io_context {
 public:
  void run() {}
  void stop() {}
};
}  // namespace boost::asio
