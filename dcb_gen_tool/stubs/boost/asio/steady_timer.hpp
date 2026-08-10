// Stub for codegen parsing only — the real header ships with Boost.Asio.
#pragma once

#include <chrono>

namespace boost::asio {

using error_code = int;

class steady_timer {
 public:
  template <typename Executor, typename Duration>
  steady_timer(Executor&, Duration) {}
  template <typename Handler>
  void async_wait(Handler&&) {}
};

}  // namespace boost::asio
