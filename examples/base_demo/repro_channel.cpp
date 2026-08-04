// Minimal reproduction for the bounded-mpsc multi-producer send-failure
// race. Stops at the first failure or stuck state and dumps diagnostics.
// Never hangs: joins with a deadline and exits if threads are stuck.

#include "dart_cpp_bridge/channel.hpp"

#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <stdexcept>

namespace {

struct OptPromiseReceiver {
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

}  // namespace

int main()
{
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 10000;
  constexpr int kTotal = kProducers * kPerProducer;

  for (int round = 0; round < 300; ++round) {
    auto [tx, rx] = co::mpsc::bounded<int>(64);
    std::atomic<bool> failed{false};
    std::atomic<int> done{0};

    std::thread threads[kProducers];
    for (int p = 0; p < kProducers; ++p) {
      threads[p] = std::thread([tx, &failed, &done]() mutable {
        for (int i = 0; i < kPerProducer; ++i) {
          bool ok = std::get<0>(*stdexec::sync_wait(tx.send(1)));
          if (!ok) {
            failed.store(true, std::memory_order_relaxed);
            std::fprintf(stderr, "[repro] send failed p=%d i=%d\n", i / kPerProducer, i);
            tx.close();  // wake other parked senders
            break;
          }
        }
        done.fetch_add(1, std::memory_order_release);
      });
    }

    int got = 0;
    bool timed_out = false;
    while (got < kTotal && !timed_out && !failed.load(std::memory_order_relaxed)) {
      std::promise<int> done_p;
      auto fut = done_p.get_future();
      auto rop = stdexec::connect(
          stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
          OptPromiseReceiver{&done_p});
      stdexec::start(rop);
      if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        timed_out = true;
      } else {
        try {
          (void)fut.get();
          ++got;
        } catch (...) {
          timed_out = true;
        }
      }
    }
    tx.close();

    // Join with deadline; never hang the harness.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (done.load(std::memory_order_acquire) < kProducers) {
      if (std::chrono::steady_clock::now() > deadline) {
        std::fprintf(stderr,
                     "[repro] STUCK: round=%d got=%d timed_out=%d failed=%d "
                     "done=%d/%d\n",
                     round, got, static_cast<int>(timed_out),
                     static_cast<int>(failed.load(std::memory_order_relaxed)),
                     done.load(std::memory_order_acquire), kProducers);
        std::fflush(stderr);
        std::exit(3);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto& t : threads) {
      t.join();
    }

    if (failed.load(std::memory_order_relaxed)) {
      std::fprintf(stderr, "[repro] SEND FAILED round=%d got=%d timed_out=%d\n",
                   round, got, static_cast<int>(timed_out));
      std::fflush(stderr);
      std::exit(1);
    }
    if (got != kTotal) {
      std::fprintf(stderr, "[repro] MISSING round=%d got=%d timed_out=%d\n",
                   round, got, static_cast<int>(timed_out));
      std::fflush(stderr);
      std::exit(2);
    }
    if (round % 50 == 0) {
      std::fprintf(stderr, "round %d ok\n", round);
    }
  }
  std::fprintf(stderr, "repro: no failure in 300 rounds\n");
  return 0;
}
