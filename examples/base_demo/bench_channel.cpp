// Channel benchmark (no Dart VM): measures co::mpsc throughput under
// multi-producer contention.
//
//   dcb_bench [bounded|unbounded|all] [producers...]
//
// Each producer thread sends `kMsgsPerProducer` values; the consumer (main
// thread) receives them all. Sends use stdexec::sync_wait on the calling
// thread (completion is inline for the fast path, or signalled by a recv
// waking the parked send), so the measurement isolates the channel itself —
// no io_context involvement.

#include "dart_cpp_bridge/channel.hpp"

#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <thread>
#include <vector>

namespace {

std::atomic<int> g_fail{0};

using Clock = std::chrono::steady_clock;

// Receiver completing a std::promise<int> with the recv value; raises on
// close/error so a timed-out recv is detectable (instead of hanging).
struct BenchRecvReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::promise<int>* done;

  void set_value(std::optional<int> v) && noexcept {
    try {
      if (!v) {
        done->set_exception(std::make_exception_ptr(std::runtime_error("closed")));
      } else {
        done->set_value(std::move(*v));
      }
    } catch (...) {
    }
  }

  void set_error(std::exception_ptr ep) && noexcept {
    try {
      done->set_exception(ep);
    } catch (...) {
    }
  }

  void set_stopped() && noexcept {
    try {
      done->set_exception(std::make_exception_ptr(std::runtime_error("stopped")));
    } catch (...) {
    }
  }
};

// Receive one value with a hard timeout; returns false on close/error/timeout.
bool recv_one(auto& rx, std::chrono::milliseconds timeout, std::atomic<bool>& timed_out)
{
  std::promise<int> done;
  auto fut = done.get_future();
  auto rop = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
      BenchRecvReceiver{&done});
  stdexec::start(rop);
  if (fut.wait_for(timeout) != std::future_status::ready) {
    timed_out.store(true, std::memory_order_relaxed);
    return false;
  }
  try {
    (void)fut.get();
    return true;
  } catch (...) {
    return false;
  }
}

template <typename Tx, typename SendOne>
double run_case(const char* name, int producers, int per_producer,
                Tx&& make_channel, SendOne send_one)
{
  auto [tx, rx] = make_channel();

  auto t0 = Clock::now();
  std::vector<std::thread> threads;
  threads.reserve(static_cast<std::size_t>(producers));
  for (int p = 0; p < producers; ++p) {
    threads.emplace_back([tx, per_producer, send_one]() mutable {
      for (int i = 0; i < per_producer; ++i) {
        send_one(tx, i);
      }
    });
  }

  const int total = producers * per_producer;
  int received = 0;
  std::atomic<bool> timed_out{false};
  while (received < total && !timed_out.load(std::memory_order_relaxed)) {
    if (!recv_one(rx, std::chrono::seconds(5), timed_out)) {
      g_fail.fetch_add(1);
      std::fprintf(stderr, "[bench] recv failed at %d/%d\n", received, total);
    } else {
      ++received;
    }
  }
  bool prod_ok = true;
  for (auto& t : threads) {
    t.join();
  }
  auto t1 = Clock::now();

  if (timed_out.load(std::memory_order_relaxed)) {
    std::fprintf(stderr, "[bench] %s: recv TIMED OUT at %d/%d\n", name,
                 received, total);
    g_fail.fetch_add(1);
  }
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  double rate = static_cast<double>(total) / (ms / 1000.0);
  std::printf("%-28s %3d   %8d   %10.1f  %12.0f\n", name, producers, total,
              ms, rate);
  std::fflush(stdout);
  return rate;
}

void bench_all(int producers, int per_producer)
{
  std::printf("%-28s %3s   %8s   %12s  %14s\n", "case", "prod", "msgs", "ms",
              "msg/s");
  std::printf("------------------------------------------------------------------\n");
  std::fflush(stdout);

  // Bounded, small capacity: heavy park/wake (backpressure) + lock contention.
  run_case("bounded cap=8", producers, per_producer,
           [] { return co::mpsc::bounded<int>(8); },
           [](auto& tx, int) {
             (void)std::get<0>(*stdexec::sync_wait(tx.send(1)));
           });

  // Bounded, large capacity: senders rarely park; lock + MPMC queue only.
  run_case("bounded cap=4096", producers, per_producer,
           [] { return co::mpsc::bounded<int>(4096); },
           [](auto& tx, int) {
             (void)std::get<0>(*stdexec::sync_wait(tx.send(1)));
           });

  // Unbounded (deque + mutex): every push/pop inside the critical section.
  run_case("unbounded (deque+mutex)", producers, per_producer,
           [] { return co::mpsc::unbounded<int>(); },
           [](auto& tx, int) {
             if (!tx.send(1)) {
               g_fail.fetch_add(1);
             }
           });
}

}  // namespace

int main(int argc, char** argv)
{
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::string mode = "all";
  std::vector<int> producers{1, 4, 8};
  int per_producer = 100000;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "bounded" || a == "unbounded") {
      mode = a;
    } else if (a.rfind("n=", 0) == 0) {
      per_producer = std::atoi(a.c_str() + 2);
    } else {
      producers.push_back(std::atoi(argv[i]));
    }
  }

  if (mode == "unbounded") {
    std::printf("%-28s %3s   %8s   %12s  %14s\n", "case", "prod", "msgs",
                "ms", "msg/s");
    std::printf("---------------------------------------------------------------\n");
    std::fflush(stdout);
    for (int p : producers) {
      run_case("unbounded (deque+mutex)", p, per_producer,
               [] { return co::mpsc::unbounded<int>(); },
               [](auto& tx, int) {
                 if (!tx.send(1)) {
                   g_fail.fetch_add(1);
                 }
               });
    }
  } else if (mode == "bounded") {
    for (int p : producers) {
      bench_all(p, per_producer);
      std::printf("\n");
      std::fflush(stdout);
    }
  } else {
    bench_all(1, per_producer);
    std::printf("\n");
    std::fflush(stdout);
    bench_all(4, per_producer);
    std::printf("\n");
    std::fflush(stdout);
    bench_all(8, per_producer);
  }

  if (g_fail.load() != 0) {
    std::fprintf(stderr, "FAIL: send/recv errors\n");
    return 1;
  }
  std::printf("bench done\n");
  return 0;
}
