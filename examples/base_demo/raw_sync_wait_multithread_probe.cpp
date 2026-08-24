#include "dart_cpp_bridge/runtime.hpp"

#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <mutex>
#include <string_view>
#include <thread>

namespace {

void fail(const char* message, int code) {
  std::fprintf(stderr, "raw multithread sync_wait probe failed: %s\n", message);
  std::_Exit(code);
}

void run_one_waiter() {
  using namespace dcb;
  Runtime::instance().set_io_threads(2);
  Runtime::instance().start();

  auto result = std::make_shared<std::promise<int>>();
  auto result_future = result->get_future();
  DCB_ASIO_NS::post(Runtime::instance().io(), [result] {
    try {
      auto value = stdexec::sync_wait(stdexec::starts_on(
          *Runtime::instance().io_scheduler(), stdexec::just(7)));
      result->set_value(std::get<0>(*value));
    } catch (...) {
      try {
        result->set_exception(std::current_exception());
      } catch (...) {
      }
    }
  });

  if (result_future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    fail("one waiter did not complete with a spare io thread", 2);
  }
  try {
    if (result_future.get() != 7) {
      fail("one waiter returned the wrong value", 3);
    }
  } catch (...) {
    fail("one waiter propagated an unexpected exception", 4);
  }

  Runtime::instance().stop();
  Runtime::instance().set_io_threads(1);
  std::printf("2 io threads: one raw sync_wait completed and released its thread\n");
}

void run_all_waiters() {
  using namespace dcb;
  Runtime::instance().set_io_threads(2);
  Runtime::instance().start();

  std::mutex mu;
  std::condition_variable cv;
  int ready = 0;
  bool go = false;
  std::atomic<int> entered_wait{0};
  std::atomic<int> finished{0};
  std::atomic<bool> marker_ran{false};
  auto both_ready = std::make_shared<std::promise<void>>();
  auto both_ready_future = both_ready->get_future();
  auto both_entered_wait = std::make_shared<std::promise<void>>();
  auto both_entered_wait_future = both_entered_wait->get_future();

  auto waiter = [&] {
    {
      std::unique_lock lock(mu);
      ++ready;
      if (ready == 2) {
        try {
          both_ready->set_value();
        } catch (...) {
        }
      }
      cv.wait(lock, [&] { return go; });
    }

    if (entered_wait.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
      try {
        both_entered_wait->set_value();
      } catch (...) {
      }
    }

    // Both io runner threads are occupied here. The operation started by
    // starts_on is queued on the same io scheduler and cannot begin.
    (void)stdexec::sync_wait(stdexec::starts_on(
        *Runtime::instance().io_scheduler(), stdexec::just(7)));
    finished.fetch_add(1, std::memory_order_release);
  };

  DCB_ASIO_NS::post(Runtime::instance().io(), waiter);
  DCB_ASIO_NS::post(Runtime::instance().io(), waiter);

  if (both_ready_future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    fail("two waiters did not start", 5);
  }
  {
    std::lock_guard lock(mu);
    go = true;
  }
  cv.notify_all();

  if (both_entered_wait_future.wait_for(std::chrono::seconds(3)) !=
      std::future_status::ready) {
    fail("two waiters did not enter raw sync_wait", 6);
  }

  DCB_ASIO_NS::post(Runtime::instance().io(), [&] { marker_ran.store(true); });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  if (finished.load(std::memory_order_acquire) != 0 ||
      marker_ran.load(std::memory_order_acquire)) {
    fail("all waiters unexpectedly made progress", 7);
  }

  std::printf("2 io threads: two raw sync_wait calls blocked the whole scheduler as expected\n");
  // Both io threads are intentionally wedged inside raw sync_wait. This is
  // an isolated child process, so terminate without trying to join them.
  std::_Exit(0);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s one|all\n", argv[0]);
    return 1;
  }

  const std::string_view mode(argv[1]);
  if (mode == "one") {
    run_one_waiter();
    return 0;
  }
  if (mode == "all") {
    run_all_waiters();
    return 0;
  }

  std::fprintf(stderr, "unknown mode: %s\n", argv[1]);
  return 1;
}
