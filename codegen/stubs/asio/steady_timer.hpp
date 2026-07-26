#pragma once
// Parse-only stub for codegen.
#include <chrono>

namespace asio {

using error_code = int;

class steady_timer {
 public:
  template <typename Executor, typename Duration>
  steady_timer(Executor&, Duration) {}
  template <typename Handler>
  void async_wait(Handler&&) {}
};

}  // namespace asio
