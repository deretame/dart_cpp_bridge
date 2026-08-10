// Stub for codegen parsing only — the real header ships with Boost.Asio.
#pragma once

namespace boost::asio {
class thread_pool {
 public:
  explicit thread_pool(unsigned int = 0) {}
  void join() {}
};
}  // namespace boost::asio
