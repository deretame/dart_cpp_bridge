// Stub for codegen parsing only — the real header is
// dart/native/include/dart_cpp_bridge/runtime.hpp. Codegen only needs the dcb
// runtime names (Runtime / IoContextScheduler / sleep / sync_wait / ...); the
// real stdexec/asio machinery (which pulls in rigtorp/MPMCQueue.h etc.) is not
// parsed. Without this stub, an API header that includes runtime.hpp falls back
// to the real header, whose dependency chain does not parse under libclang and
// silently degrades template types (std::vector etc.) to `int`.
#pragma once

#include "dart_cpp_bridge/codec.hpp"

#include <stdexec/execution.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dcb {

struct Unit {};

class IoContextScheduler {
 public:
  bool current_thread_is_io() const noexcept;
};

using DartPostFn = void (*)(std::int64_t port, const std::uint8_t* data,
                            std::size_t len, void* userdata);

class Runtime {
 public:
  static Runtime& instance();

  void start();
  void stop();
  bool running() const;
  void set_pool_threads(std::uint32_t n);
  IoContextScheduler* io_scheduler();
  void set_dart_post(DartPostFn fn, void* userdata);
  void post_to_dart(std::int64_t port, const std::uint8_t* data, std::size_t len);
};

inline IoContextScheduler* io_scheduler() {
  return Runtime::instance().io_scheduler();
}

// Timed sleep on the io scheduler (co_await dcb::sleep(...)).
template <typename Rep, typename Period>
stdexec::task<void> sleep(std::chrono::duration<Rep, Period> dur);

// Deadlock-guarded sync wait for non-coroutine contexts.
template <typename S>
void sync_wait(S&& sndr);

// Fire-and-forget launch of a sender on the io scheduler.
template <typename S>
void launch_on_io(S&& sndr);

}  // namespace dcb
