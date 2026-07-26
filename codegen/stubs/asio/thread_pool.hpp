#pragma once
// Parse-only stub for codegen.
namespace asio {
class thread_pool {
 public:
  explicit thread_pool(unsigned int = 0) {}
  void join() {}
};
}  // namespace asio
