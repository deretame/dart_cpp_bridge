#include "dart_cpp_bridge/runtime.hpp"

#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>

namespace {

void fail(const char* message, int code) {
  std::fprintf(stderr, "raw sync_wait probe failed: %s\n", message);
  std::_Exit(code);
}

}  // namespace

int main() {
  using namespace dcb;
  setvbuf(stdout, nullptr, _IONBF, 0);

  Runtime::instance().start();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::atomic<bool> finished{false};

  DCB_ASIO_NS::post(Runtime::instance().io(), [&] {
    try {
      entered.set_value();
    } catch (...) {
      // The promise is only used to synchronize the probe's main thread.
    }

    // This deliberately uses the raw stdexec API. There is no dcb::sync_wait
    // deadlock guard here, so starting work on this same single-threaded
    // scheduler makes the scheduler unable to process the queued operation.
    (void)stdexec::sync_wait(stdexec::starts_on(
        *Runtime::instance().io_scheduler(), stdexec::just(1)));
    finished.store(true, std::memory_order_release);
  });

  if (entered_future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    fail("probe handler did not start", 2);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  if (finished.load(std::memory_order_acquire)) {
    fail("raw stdexec::sync_wait unexpectedly completed on the io thread", 3);
  }

  std::printf("raw stdexec::sync_wait blocked the io scheduler as expected\n");
  // The io thread is intentionally wedged inside raw sync_wait. This probe is
  // an isolated child process, so terminate without trying to join that thread.
  std::_Exit(0);
}
