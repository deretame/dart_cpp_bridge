// C++ unit tests for the co::oneshot / co::mpsc channels, run under doctest.
//
// The channel primitives need no Runtime / io_context: all senders complete
// inline or are woken by a peer thread, so the tests use stdexec::sync_wait
// and starts_on(inline_scheduler) directly. The final section drives the
// channel from stdexec::task coroutines — the shape business code actually
// uses (co_await send()/recv(), Stream combinators, stop-token cancel).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "dart_cpp_bridge/channel.hpp"

#include <stdexec/execution.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// Receiver completing a std::promise<bool> (bounded send result).
struct BoolPromiseReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::promise<bool>* done;

  void set_value(bool v) && noexcept {
    try {
      done->set_value(v);
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

// Receiver completing a std::promise<T> with a recv value; raises
// "channel closed" on nullopt (oneshot / mpsc recv semantics).
template <typename T>
struct OptPromiseReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::promise<T>* done;

  void set_value(std::optional<T> v) && noexcept {
    try {
      if (!v) {
        done->set_exception(std::make_exception_ptr(std::runtime_error("channel closed")));
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

// Bounded send that blocks the calling thread for its result.
bool sync_send_bounded(auto& tx, int v)
{
  return std::get<0>(*stdexec::sync_wait(tx.send(v)));
}

// Receive one value with a hard timeout (the underlying recv() parks
// forever otherwise). Returns the value, or -1 on timeout.
int recv_value(auto& rx, std::chrono::milliseconds timeout, bool& timed_out)
{
  std::promise<int> done;
  auto fut = done.get_future();
  auto rop = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
      OptPromiseReceiver<int>{&done});
  stdexec::start(rop);
  if (fut.wait_for(timeout) != std::future_status::ready) {
    timed_out = true;
    return -1;
  }
  return fut.get();
}

// Receive expecting the channel to report closed (throws "channel closed").
bool recv_expect_closed(auto& rx)
{
  bool timed_out = false;
  try {
    (void)recv_value(rx, std::chrono::seconds(3), timed_out);
    return false;  // got a value, channel should have been closed
  } catch (const std::exception& e) {
    return std::string(e.what()) == "channel closed" && !timed_out;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// oneshot
// ---------------------------------------------------------------------------

TEST_CASE("oneshot value and close")
{
  auto [tx, rx] = co::oneshot::channel<int>();
  CHECK(tx.send(42));
  auto v = std::get<0>(*stdexec::sync_wait(std::move(rx)));
  REQUIRE(v.has_value());
  CHECK(*v == 42);

  auto [tx2, rx2] = co::oneshot::channel<int>();
  tx2.close();
  auto v2 = std::get<0>(*stdexec::sync_wait(std::move(rx2)));
  CHECK(!v2.has_value());
}

TEST_CASE("oneshot cross-thread wake")
{
  auto [tx, rx] = co::oneshot::channel<int>();
  std::promise<int> done;
  auto fut = done.get_future();
  auto rop = stdexec::connect(std::move(rx), OptPromiseReceiver<int>{&done});
  stdexec::start(rop);

  std::thread sender([tx = std::move(tx)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)tx.send(7);
  });
  sender.join();

  REQUIRE(fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  CHECK(fut.get() == 7);
}

// ---------------------------------------------------------------------------
// mpsc unbounded
// ---------------------------------------------------------------------------

TEST_CASE("mpsc unbounded basic and close drain")
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  CHECK(tx.send(1));
  CHECK(tx.send(2));
  CHECK(tx.send(3));

  CHECK(rx.try_recv() == std::optional<int>(1));
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(3), timed_out) == 2);
  CHECK(!timed_out);
  CHECK(recv_value(rx, std::chrono::seconds(3), timed_out) == 3);
  CHECK(!timed_out);

  tx.close();
  CHECK(!tx.send(4));
  CHECK(recv_expect_closed(rx));
}

TEST_CASE("mpsc unbounded cross-thread completeness")
{
  constexpr int kPerProducer = 5000;
  constexpr int kTotal = 2 * kPerProducer;
  auto [tx, rx] = co::mpsc::unbounded<int>();
  std::atomic<int> next{1};
  std::atomic<bool> prod_failed{false};

  auto producer = [&tx, &next, &prod_failed]() {
    for (int i = 0; i < kPerProducer; ++i) {
      int n = next.fetch_add(1, std::memory_order_relaxed);
      if (!tx.send(n)) {
        prod_failed.store(true, std::memory_order_relaxed);
      }
    }
  };
  std::thread p1(producer);
  std::thread p2(producer);

  std::vector<bool> seen(static_cast<std::size_t>(kTotal) + 1, false);
  int bad = 0;
  bool timed_out = false;
  for (int i = 0; i < kTotal; ++i) {
    int v = recv_value(rx, std::chrono::seconds(5), timed_out);
    if (timed_out || v < 1 || v > kTotal || seen[static_cast<std::size_t>(v)]) {
      ++bad;
    } else {
      seen[static_cast<std::size_t>(v)] = true;
    }
  }
  p1.join();
  p2.join();

  for (int v = 1; v <= kTotal; ++v) {
    if (!seen[static_cast<std::size_t>(v)]) {
      ++bad;
    }
  }
  CHECK(bad == 0);
  CHECK(!prod_failed.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// mpsc bounded (backpressure)
// ---------------------------------------------------------------------------

TEST_CASE("mpsc bounded backpressure")
{
  auto [tx, rx] = co::mpsc::bounded<int>(2);
  CHECK(sync_send_bounded(tx, 1));
  CHECK(sync_send_bounded(tx, 2));
  CHECK(tx.remaining_capacity() == 0);

  // Third send must park until a receiver frees a slot.
  std::promise<bool> send3;
  auto fut3 = send3.get_future();
  auto op3 = stdexec::connect(tx.send(3), BoolPromiseReceiver{&send3});
  stdexec::start(op3);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(fut3.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

  // recv(1) frees a slot -> parked send(3) completes with true.
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(3), timed_out) == 1);
  REQUIRE(fut3.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  CHECK(fut3.get());

  // FIFO drain: 2, then 3.
  CHECK(recv_value(rx, std::chrono::seconds(3), timed_out) == 2);
  CHECK(recv_value(rx, std::chrono::seconds(3), timed_out) == 3);

  // Close semantics: sends fail, drained recv reports closed.
  tx.close();
  CHECK(!sync_send_bounded(tx, 4));
  CHECK(recv_expect_closed(rx));
}

TEST_CASE("mpsc bounded close wakes parked sender with failure")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  CHECK(sync_send_bounded(tx, 1));

  std::promise<bool> send2;
  auto fut2 = send2.get_future();
  auto op2 = stdexec::connect(tx.send(2), BoolPromiseReceiver{&send2});
  stdexec::start(op2);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(fut2.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

  tx.close();
  REQUIRE(fut2.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  CHECK(!fut2.get());  // false: value dropped on close
}

TEST_CASE("mpsc bounded rendezvous capacity=0")
{
  auto [tx, rx] = co::mpsc::bounded<int>(0);
  std::promise<bool> send1;
  auto fut1 = send1.get_future();
  auto op1 = stdexec::connect(tx.send(1), BoolPromiseReceiver{&send1});
  stdexec::start(op1);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  CHECK(fut1.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(3), timed_out) == 1);
  REQUIRE(fut1.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  CHECK(fut1.get());
}

TEST_CASE("mpsc bounded cross-thread completeness")
{
  constexpr int kPerProducer = 5000;
  constexpr int kTotal = 2 * kPerProducer;
  auto [tx, rx] = co::mpsc::bounded<int>(8);
  std::atomic<int> next{1};
  std::atomic<bool> prod_failed{false};

  auto producer = [&tx, &next, &prod_failed]() {
    for (int i = 0; i < kPerProducer; ++i) {
      int n = next.fetch_add(1, std::memory_order_relaxed);
      bool ok = std::get<0>(*stdexec::sync_wait(tx.send(n)));
      if (!ok) {
        prod_failed.store(true, std::memory_order_relaxed);
      }
    }
  };
  std::thread p1(producer);
  std::thread p2(producer);

  std::vector<bool> seen(static_cast<std::size_t>(kTotal) + 1, false);
  int bad = 0;
  bool timed_out = false;
  for (int i = 0; i < kTotal; ++i) {
    int v = recv_value(rx, std::chrono::seconds(5), timed_out);
    if (timed_out || v < 1 || v > kTotal || seen[static_cast<std::size_t>(v)]) {
      ++bad;
    } else {
      seen[static_cast<std::size_t>(v)] = true;
    }
  }
  p1.join();
  p2.join();

  for (int v = 1; v <= kTotal; ++v) {
    if (!seen[static_cast<std::size_t>(v)]) {
      ++bad;
    }
  }
  CHECK(bad == 0);
  CHECK(!prod_failed.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Throughput sanity (kept in the test flow; dcb_bench is the detailed tool)
// ---------------------------------------------------------------------------

TEST_CASE("mpsc bounded throughput sanity")
{
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 10000;
  constexpr int kTotal = kProducers * kPerProducer;
  auto [tx, rx] = co::mpsc::bounded<int>(64);
  std::atomic<bool> prod_failed{false};

  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for (int p = 0; p < kProducers; ++p) {
    threads.emplace_back([tx, &prod_failed]() mutable {
      for (int i = 0; i < kPerProducer; ++i) {
        bool ok = std::get<0>(*stdexec::sync_wait(tx.send(1)));
        if (!ok) {
          prod_failed.store(true, std::memory_order_relaxed);
        }
      }
    });
  }

  int got = 0;
  bool timed_out = false;
  while (got < kTotal && !timed_out) {
    (void)recv_value(rx, std::chrono::seconds(5), timed_out);
    if (!timed_out) {
      ++got;
    }
  }
  for (auto& t : threads) {
    t.join();
  }
  auto t1 = std::chrono::steady_clock::now();

  double rate = static_cast<double>(kTotal) /
                std::chrono::duration<double>(t1 - t0).count();
  INFO("4-producer bounded throughput: " << static_cast<long long>(rate)
                                         << " msg/s");
  INFO("got=" << got << " kTotal=" << kTotal
              << " timed_out=" << timed_out
              << " prod_failed=" << prod_failed.load(std::memory_order_relaxed));
  CHECK(got == kTotal);
  CHECK(!timed_out);
  CHECK(!prod_failed.load(std::memory_order_relaxed));
  // Very loose floor so slow CI machines do not flake; dcb_bench measures
  // precise numbers on demand.
  CHECK(rate > 50000.0);
}

// ---------------------------------------------------------------------------
// Cancellation: parked opstates destroyed mid-wait
// ---------------------------------------------------------------------------

namespace {

// Receiver that only counts completions. For a cancelled wait the expected
// count is zero: any completion firing into a destroyed opstate is a
// use-after-destroy and shows up here (or as a crash).
struct CountingReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::atomic<int>* values;
  std::atomic<int>* errors;

  void set_value(std::optional<int>) && noexcept {
    values->fetch_add(1, std::memory_order_relaxed);
  }

  void set_error(std::exception_ptr) && noexcept {
    errors->fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() && noexcept {
    errors->fetch_add(1, std::memory_order_relaxed);
  }
};

// Park one recv() and then cancel it by destroying the opstate mid-wait.
// With inline_scheduler the recv() body runs synchronously inside start()
// until it suspends, so the opstate is guaranteed parked when start()
// returns (given the channel is empty).
void park_and_cancel_recv(auto& rx, std::atomic<int>& v, std::atomic<int>& e)
{
  auto op = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
      CountingReceiver{&v, &e});
  stdexec::start(op);
}

}  // namespace

TEST_CASE("oneshot cancel parked receiver then send")
{
  auto [tx, rx] = co::oneshot::channel<int>();
  std::atomic<int> values{0}, errors{0};
  {
    auto op = stdexec::connect(std::move(rx), CountingReceiver{&values, &errors});
    stdexec::start(op);  // parks: channel is empty
  }  // cancel: destroy the parked opstate

  // The only waiter was cancelled; send() must report detached (false)
  // instead of writing a value nobody will ever read.
  CHECK(!tx.send(42));
  CHECK(values.load(std::memory_order_relaxed) == 0);
  CHECK(errors.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("mpsc bounded recv cancel preserves the value")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  std::atomic<int> values{0}, errors{0};
  park_and_cancel_recv(rx, values, errors);

  // The cancelled waiter must not swallow the value: send must buffer it.
  CHECK(sync_send_bounded(tx, 42));
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 42);
  CHECK(!timed_out);
  CHECK(values.load(std::memory_order_relaxed) == 0);
  CHECK(errors.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("mpsc bounded repeated recv cancel preserves values")
{
  auto [tx, rx] = co::mpsc::bounded<int>(2);
  std::atomic<int> values{0}, errors{0};
  park_and_cancel_recv(rx, values, errors);
  park_and_cancel_recv(rx, values, errors);

  CHECK(sync_send_bounded(tx, 1));
  CHECK(sync_send_bounded(tx, 2));
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 1);
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 2);
  CHECK(!timed_out);
  CHECK(values.load(std::memory_order_relaxed) == 0);
  CHECK(errors.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("mpsc unbounded recv cancel preserves the value")
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  std::atomic<int> values{0}, errors{0};
  park_and_cancel_recv(rx, values, errors);

  CHECK(tx.send(7));
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 7);
  CHECK(!timed_out);
  CHECK(values.load(std::memory_order_relaxed) == 0);
  CHECK(errors.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("mpsc bounded send cancel withdraws the value")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  CHECK(sync_send_bounded(tx, 1));  // channel full
  std::promise<bool> send2;
  auto fut2 = send2.get_future();
  {
    auto op2 = stdexec::connect(tx.send(2), BoolPromiseReceiver{&send2});
    stdexec::start(op2);  // parks: value 2 stays inside the opstate
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(fut2.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
  }  // cancel the parked send: the opstate is destroyed mid-wait

  // Withdrawal semantics (tokio cancel-safety): a cancelled parked send never
  // reaches the channel — the value is destroyed with the operation state.
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 1);
  CHECK(!timed_out);
  CHECK(fut2.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
  tx.close();
  CHECK(recv_expect_closed(rx));  // no 2 is ever delivered
}

TEST_CASE("mpsc bounded send cancel preserves the FIFO of the rest")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  CHECK(sync_send_bounded(tx, 1));  // buffer: [1]

  std::promise<bool> p2, p3, p4;
  auto fut2 = p2.get_future();
  auto fut3 = p3.get_future();
  auto fut4 = p4.get_future();
  auto op2 = stdexec::connect(tx.send(2), BoolPromiseReceiver{&p2});
  auto op4 = stdexec::connect(tx.send(4), BoolPromiseReceiver{&p4});
  stdexec::start(op2);  // queued: head
  {
    auto op3 = stdexec::connect(tx.send(3), BoolPromiseReceiver{&p3});
    stdexec::start(op3);  // queued: middle
    stdexec::start(op4);  // queued: tail
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  }  // cancel the middle parked send: op3 destroyed, value 3 is withdrawn

  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 1);
  REQUIRE(fut2.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(fut2.get());
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 2);
  REQUIRE(fut4.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(fut4.get());
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 4);
  CHECK(!timed_out);
  // The cancelled send never completed (its value was withdrawn).
  CHECK(fut3.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
  tx.close();
  CHECK(recv_expect_closed(rx));
}

TEST_CASE("mpsc bounded close after recv cancel")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  std::atomic<int> values{0}, errors{0};
  park_and_cancel_recv(rx, values, errors);

  tx.close();
  CHECK(!sync_send_bounded(tx, 1));
  CHECK(recv_expect_closed(rx));
  CHECK(values.load(std::memory_order_relaxed) == 0);
  CHECK(errors.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("mpsc bounded recv cancel loop")
{
  // Deterministic single-threaded cancel / send / recv churn. Every value
  // sent after a cancel must still be received.
  constexpr int kIterations = 2000;
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  int received = 0;
  for (int i = 0; i < kIterations; ++i) {
    std::atomic<int> v{0}, e{0};
    park_and_cancel_recv(rx, v, e);  // parks (empty channel), then cancels
    CHECK(v.load(std::memory_order_relaxed) == 0);
    CHECK(e.load(std::memory_order_relaxed) == 0);
    CHECK(sync_send_bounded(tx, i));
    bool timed_out = false;
    int got = recv_value(rx, std::chrono::milliseconds(200), timed_out);
    if (timed_out) {
      CHECK(got == i);  // fails: value lost into the cancelled waiter
      break;
    }
    CHECK(got == i);
    ++received;
  }
  CHECK(received == kIterations);
}

TEST_CASE("mpsc bounded recv cancel concurrent stress")
{
  // Producers send at full speed while the consumer keeps parking and
  // cancelling recvs. Every accepted send must be accounted for: values
  // delivered inline are counted immediately, everything else is drained
  // after the producers stop. A lost value (or a crash) fails the test.
  constexpr int kIterations = 3000;
  auto [tx, rx] = co::mpsc::bounded<int>(4);
  std::atomic<long> sent{0};
  std::atomic<bool> stop{false};

  auto producer = [&tx, &sent, &stop]() {
    while (!stop.load(std::memory_order_relaxed)) {
      bool ok = std::get<0>(*stdexec::sync_wait(tx.send(1)));
      if (!ok) {
        break;  // closed while parked
      }
      sent.fetch_add(1, std::memory_order_relaxed);
    }
  };
  std::thread p1(producer);
  std::thread p2(producer);

  long received = 0;
  for (int i = 0; i < kIterations; ++i) {
    std::atomic<int> v{0}, e{0};
    {
      auto op = stdexec::connect(
          stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
          CountingReceiver{&v, &e});
      stdexec::start(op);
      if (i % 4 == 3) {
        // Occasionally give producers a head start so the recv really parks
        // before being cancelled.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }  // destroy first (cancels a parked recv), then count completions
    received += v.load(std::memory_order_relaxed);
  }
  stop.store(true, std::memory_order_relaxed);
  tx.close();
  p1.join();
  p2.join();

  while (true) {
    auto v = rx.try_recv();
    if (!v) {
      break;
    }
    ++received;
  }
  INFO("sent=" << sent.load(std::memory_order_relaxed)
               << " received=" << received);
  CHECK(received == sent.load(std::memory_order_relaxed));
}

// ---------------------------------------------------------------------------
// Single-consumer enforcement
// ---------------------------------------------------------------------------

TEST_CASE("mpsc single-consumer violation fails the offending recv")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);

  // Consumer A parks (channel is empty).
  std::promise<int> a;
  auto futa = a.get_future();
  auto opa = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
      OptPromiseReceiver<int>{&a});
  stdexec::start(opa);  // parked

  // Consumer B calls recv() while A is still parked: the new recv() must
  // fail loudly instead of silently clobbering A's wait with a spurious
  // "channel closed".
  std::promise<int> b;
  auto futb = b.get_future();
  auto opb = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
      OptPromiseReceiver<int>{&b});
  stdexec::start(opb);
  try {
    (void)futb.get();
    FAIL("expected logic_error for concurrent recv()");
  } catch (const std::logic_error& e) {
    CHECK(std::string(e.what()).find("single-consumer") != std::string::npos);
  }

  // A's parked wait is untouched: a value sent now is delivered to A.
  CHECK(sync_send_bounded(tx, 7));
  REQUIRE(futa.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(futa.get() == 7);
}

TEST_CASE("mpsc sequential recv after cancel is not a violation")
{
  // A cancelled (detached) parked recv must NOT trip the single-consumer
  // check: the next recv() parks normally.
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  std::atomic<int> v{0}, e{0};
  park_and_cancel_recv(rx, v, e);

  std::promise<int> b;
  auto futb = b.get_future();
  auto opb = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{}, rx.recv()),
      OptPromiseReceiver<int>{&b});
  stdexec::start(opb);  // must park, not throw

  CHECK(sync_send_bounded(tx, 9));
  REQUIRE(futb.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  CHECK(futb.get() == 9);
}

// ---------------------------------------------------------------------------
// Stop token cancellation (stdexec::inplace_stop_source)
// ---------------------------------------------------------------------------
//
// Deterministic cancellation tests: no timing races — request_stop() fires
// the registered callback inline on the requesting thread. Lifetime rule:
// the operation state (which holds the callback registration) must be
// destroyed before the inplace_stop_source.

namespace {

// Receiver with an inplace stop token in its environment, counting bounded
// send outcomes (true = accepted, false = closed, stopped, error).
struct StopBoolReceiver {
  using receiver_concept = stdexec::receiver_tag;

  stdexec::inplace_stop_token tok;
  std::atomic<int>* value_true;
  std::atomic<int>* value_false;
  std::atomic<int>* stopped;
  std::atomic<int>* errors;

  auto get_env() const noexcept
  {
    return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
  }

  void set_value(bool v) && noexcept
  {
    (v ? *value_true : *value_false).fetch_add(1, std::memory_order_relaxed);
  }

  void set_error(std::exception_ptr) && noexcept
  {
    errors->fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() && noexcept
  {
    stopped->fetch_add(1, std::memory_order_relaxed);
  }
};

// Same, for recv outcomes: values (engaged optional), closed (disengaged),
// stopped, error.
struct StopOptReceiver {
  using receiver_concept = stdexec::receiver_tag;

  stdexec::inplace_stop_token tok;
  std::atomic<int>* values;
  std::atomic<int>* closed;
  std::atomic<int>* stopped;
  std::atomic<int>* errors;

  auto get_env() const noexcept
  {
    return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
  }

  void set_value(std::optional<int> v) && noexcept
  {
    (v ? *values : *closed).fetch_add(1, std::memory_order_relaxed);
  }

  void set_error(std::exception_ptr) && noexcept
  {
    errors->fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() && noexcept
  {
    stopped->fetch_add(1, std::memory_order_relaxed);
  }
};

}  // namespace

TEST_CASE("mpsc bounded send stop token withdraws the parked value")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  CHECK(sync_send_bounded(tx, 1));  // channel full

  stdexec::inplace_stop_source src;
  std::atomic<int> v_true{0}, v_false{0}, stopped{0}, errors{0};
  {
    auto op = stdexec::connect(
        tx.send(2), StopBoolReceiver{src.get_token(), &v_true, &v_false,
                                     &stopped, &errors});
    stdexec::start(op);  // parks (channel full)
    CHECK(stopped.load(std::memory_order_relaxed) == 0);
    src.request_stop();  // fires the callback inline on this thread
    CHECK(stopped.load(std::memory_order_relaxed) == 1);
    CHECK(v_true.load(std::memory_order_relaxed) == 0);
    CHECK(v_false.load(std::memory_order_relaxed) == 0);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
  }  // the opstate (callback registration) is destroyed before `src`

  // The value was withdrawn: only 1 is ever delivered.
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 1);
  CHECK(!timed_out);
  tx.close();
  CHECK(recv_expect_closed(rx));
}

TEST_CASE("mpsc bounded send stop requested before start completes stopped")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  CHECK(sync_send_bounded(tx, 1));  // channel full: send(2) would park

  stdexec::inplace_stop_source src;
  src.request_stop();  // before connect/start

  std::atomic<int> v_true{0}, v_false{0}, stopped{0}, errors{0};
  {
    auto op = stdexec::connect(
        tx.send(2), StopBoolReceiver{src.get_token(), &v_true, &v_false,
                                     &stopped, &errors});
    stdexec::start(op);  // the callback fires inline during registration
    CHECK(stopped.load(std::memory_order_relaxed) == 1);
    CHECK(v_true.load(std::memory_order_relaxed) == 0);
    CHECK(v_false.load(std::memory_order_relaxed) == 0);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
  }

  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 1);
  CHECK(!timed_out);
  tx.close();
  CHECK(recv_expect_closed(rx));
}

TEST_CASE("mpsc recv stop token cancels the parked wait")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);

  stdexec::inplace_stop_source src;
  std::atomic<int> values{0}, closed_n{0}, stopped{0}, errors{0};
  {
    auto op = stdexec::connect(
        rx.recv_raw(),
        StopOptReceiver{src.get_token(), &values, &closed_n, &stopped,
                        &errors});
    stdexec::start(op);  // parks (channel empty)
    src.request_stop();
    CHECK(stopped.load(std::memory_order_relaxed) == 1);
    CHECK(values.load(std::memory_order_relaxed) == 0);
    CHECK(closed_n.load(std::memory_order_relaxed) == 0);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
  }

  // The channel is unaffected: values flow, and a later recv parks normally.
  CHECK(sync_send_bounded(tx, 7));
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 7);
  CHECK(!timed_out);
  tx.close();
  CHECK(recv_expect_closed(rx));
}

TEST_CASE("mpsc unbounded recv stop requested before start completes stopped")
{
  auto [tx, rx] = co::mpsc::unbounded<int>();  // cover the unbounded path too

  stdexec::inplace_stop_source src;
  src.request_stop();  // before connect/start

  std::atomic<int> values{0}, closed_n{0}, stopped{0}, errors{0};
  {
    auto op = stdexec::connect(
        rx.recv_raw(),
        StopOptReceiver{src.get_token(), &values, &closed_n, &stopped,
                        &errors});
    stdexec::start(op);
    CHECK(stopped.load(std::memory_order_relaxed) == 1);
    CHECK(values.load(std::memory_order_relaxed) == 0);
    CHECK(closed_n.load(std::memory_order_relaxed) == 0);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
  }

  CHECK(tx.send(9));
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 9);
  CHECK(!timed_out);
  tx.close();
  CHECK(recv_expect_closed(rx));
}

TEST_CASE("stdexec::task forwards stop into a parked channel await")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  CHECK(sync_send_bounded(tx, 1));  // full: the co_awaited send(2) will park

  stdexec::inplace_stop_source src;
  std::atomic<int> resumed{0};  // statements after the co_await
  std::atomic<int> values{0}, errors{0}, stopped{0};

  auto worker = [&]() -> stdexec::task<void> {
    (void)co_await tx.send(2);
    resumed.fetch_add(1, std::memory_order_relaxed);  // must not run on stop
  };

  // Receiver for the task itself; write_env injects the stop token into the
  // task's environment, and the task forwards it to the awaited channel op.
  struct TaskReceiver {
    using receiver_concept = stdexec::receiver_tag;
    std::atomic<int>* values;
    std::atomic<int>* errors;
    std::atomic<int>* stopped;

    void set_value() && noexcept
    {
      values->fetch_add(1, std::memory_order_relaxed);
    }

    void set_error(std::exception_ptr) && noexcept
    {
      errors->fetch_add(1, std::memory_order_relaxed);
    }

    void set_stopped() && noexcept
    {
      stopped->fetch_add(1, std::memory_order_relaxed);
    }
  };

  {
    auto op = stdexec::connect(
        stdexec::starts_on(
            stdexec::inline_scheduler{},
            stdexec::write_env(
                worker(),
                stdexec::prop{stdexec::get_stop_token, src.get_token()})),
        TaskReceiver{&values, &errors, &stopped});
    stdexec::start(op);
    src.request_stop();

    // stdexec::task propagates the channel's set_stopped WITHOUT resuming the
    // coroutine (symmetric transfer to unhandled_stopped): the co_await
    // never produces a value and the task completes stopped.
    CHECK(stopped.load(std::memory_order_relaxed) == 1);
    CHECK(resumed.load(std::memory_order_relaxed) == 0);
    CHECK(values.load(std::memory_order_relaxed) == 0);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
  }

  // The parked send was cancelled through the token: value 2 was withdrawn.
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 1);
  CHECK(!timed_out);
  tx.close();
  CHECK(recv_expect_closed(rx));
}

namespace {

// Receiver for the send stop stress: records the outcome kind and signals
// completion. kind: 1 = accepted, 2 = closed, 3 = stopped, 4 = error.
struct StressSendReceiver {
  using receiver_concept = stdexec::receiver_tag;

  stdexec::inplace_stop_token tok;
  std::atomic<int>* kind;
  std::atomic<bool>* done;

  auto get_env() const noexcept
  {
    return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
  }

  void finish(int k) noexcept
  {
    kind->store(k, std::memory_order_release);
    done->store(true, std::memory_order_release);
  }

  void set_value(bool ok) && noexcept { finish(ok ? 1 : 2); }

  void set_error(std::exception_ptr) && noexcept { finish(4); }

  void set_stopped() && noexcept { finish(3); }
};

// Receiver for the recv stop stress. kind: 1 = value, 2 = closed,
// 3 = stopped, 4 = error.
struct StressRecvReceiver {
  using receiver_concept = stdexec::receiver_tag;

  stdexec::inplace_stop_token tok;
  std::atomic<int>* kind;
  std::atomic<int>* value;
  std::atomic<bool>* done;

  auto get_env() const noexcept
  {
    return stdexec::env{stdexec::prop{stdexec::get_stop_token, tok}};
  }

  void finish(int k) noexcept
  {
    kind->store(k, std::memory_order_release);
    done->store(true, std::memory_order_release);
  }

  void set_value(std::optional<int> v) && noexcept
  {
    if (v) {
      value->store(*v, std::memory_order_release);
      finish(1);
    } else {
      finish(2);
    }
  }

  void set_error(std::exception_ptr) && noexcept { finish(4); }

  void set_stopped() && noexcept { finish(3); }
};

// Spin until `done` (bounded); self-cancels via `src` after 5s so a bug
// shows up as a test failure instead of a hung test binary.
bool wait_done(std::atomic<bool>& done, stdexec::inplace_stop_source& src)
{
  const auto t0 = std::chrono::steady_clock::now();
  bool cancelled = false;
  while (!done.load(std::memory_order_acquire)) {
    const auto waited = std::chrono::steady_clock::now() - t0;
    if (!cancelled && waited > std::chrono::seconds(5)) {
      src.request_stop();  // unstick: completes stopped (or a value wins)
      cancelled = true;
    }
    if (waited > std::chrono::seconds(15)) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

TEST_CASE("mpsc bounded send stop stress conserves values")
{
  // Producers race parked sends against stop requests; the consumer drains.
  // Conservation: a send completes true ⟺ its value is received exactly
  // once; stopped/closed sends never reach the channel.
  constexpr int kProducers = 4;
  constexpr int kPerProducer = 2000;
  auto [tx, rx] = co::mpsc::bounded<int>(8);

  std::atomic<int> next{1};
  std::atomic<int> producers_done{0};
  std::mutex acc_mu;
  std::unordered_map<int, int> accepted;
  std::atomic<int> error_kinds{0};

  auto producer = [&]() {
    for (int i = 0; i < kPerProducer; ++i) {
      int v = next.fetch_add(1, std::memory_order_relaxed);
      stdexec::inplace_stop_source src;
      std::atomic<int> kind{0};
      std::atomic<bool> done{false};
      {
        auto op = stdexec::connect(
            tx.send(v), StressSendReceiver{src.get_token(), &kind, &done});
        stdexec::start(op);
        if (i % 5 == 2) {
          src.request_stop();  // ~20%: cancel the (possibly parked) send
        }
        while (!done.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
      }  // opstate destroyed before src
      int k = kind.load(std::memory_order_relaxed);
      if (k == 1) {
        std::lock_guard lock(acc_mu);
        accepted[v]++;
      } else if (k != 2 && k != 3) {
        error_kinds.fetch_add(1, std::memory_order_relaxed);
      }
    }
    producers_done.fetch_add(1, std::memory_order_release);
  };
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back(producer);
  }

  std::unordered_map<int, int> received;
  while (producers_done.load(std::memory_order_acquire) < kProducers) {
    bool timed_out = false;
    int v = recv_value(rx, std::chrono::seconds(10), timed_out);
    if (timed_out) {
      FAIL("consumer timed out while producers are still running");
      break;
    }
    received[v]++;
  }
  tx.close();
  for (;;) {  // drain: accepted values buffered before close must arrive
    bool timed_out = false;
    try {
      int v = recv_value(rx, std::chrono::seconds(10), timed_out);
      if (timed_out) {
        FAIL("drain timed out");
        break;
      }
      received[v]++;
    } catch (const std::runtime_error&) {
      break;  // channel closed & drained
    }
  }
  for (auto& t : producers) {
    t.join();
  }

  CHECK(error_kinds.load(std::memory_order_relaxed) == 0);
  INFO("accepted=" << accepted.size() << " received=" << received.size());
  CHECK(received == accepted);
}

TEST_CASE("mpsc recv stop stress keeps every value")
{
  // The consumer keeps parking recvs and cancelling ~half of them while
  // producers send at full speed. Conservation: every accepted value is
  // received exactly once (by a non-cancelled recv or the final drain).
  constexpr int kProducers = 2;
  constexpr int kPerProducer = 1500;
  auto [tx, rx] = co::mpsc::bounded<int>(4);

  std::atomic<int> next{1};
  std::atomic<int> producers_done{0};
  std::mutex acc_mu;
  std::unordered_map<int, int> accepted;

  auto producer = [&]() {
    for (int i = 0; i < kPerProducer; ++i) {
      int v = next.fetch_add(1, std::memory_order_relaxed);
      bool ok = std::get<0>(*stdexec::sync_wait(tx.send(v)));
      if (ok) {
        std::lock_guard lock(acc_mu);
        accepted[v]++;
      }
    }
    producers_done.fetch_add(1, std::memory_order_release);
  };
  std::vector<std::thread> producers;
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back(producer);
  }

  std::unordered_map<int, int> received;
  bool flip = false;
  while (producers_done.load(std::memory_order_acquire) < kProducers) {
    stdexec::inplace_stop_source src;
    std::atomic<int> kind{0};
    std::atomic<int> value{-1};
    std::atomic<bool> done{false};
    {
      auto op = stdexec::connect(
          rx.recv_raw(),
          StressRecvReceiver{src.get_token(), &kind, &value, &done});
      stdexec::start(op);
      flip = !flip;
      if (flip) {
        src.request_stop();  // cancel ~half of the parked recvs
      }
      if (!wait_done(done, src)) {
        FAIL("recv wait stuck");
        break;
      }
    }  // opstate destroyed before src
    if (kind.load(std::memory_order_relaxed) == 1) {
      received[value.load(std::memory_order_relaxed)]++;
    }
  }
  tx.close();
  for (;;) {  // drain what the cancelled recvs left behind
    bool timed_out = false;
    try {
      int v = recv_value(rx, std::chrono::seconds(10), timed_out);
      if (timed_out) {
        FAIL("drain timed out");
        break;
      }
      received[v]++;
    } catch (const std::runtime_error&) {
      break;  // channel closed & drained
    }
  }
  for (auto& t : producers) {
    t.join();
  }

  INFO("accepted=" << accepted.size() << " received=" << received.size());
  CHECK(received == accepted);
}

// ---------------------------------------------------------------------------
// Coroutine-style usage (stdexec::task)
// ---------------------------------------------------------------------------
//
// The sections above drive the channel through raw connect/start or
// sync_wait. These drive it the way business code actually uses it:
// producer/consumer coroutines co_awaiting send()/recv() directly.

namespace {

// Receiver completing a std::promise<void> (task / when_all completion).
struct VoidPromiseReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::promise<void>* done;

  void set_value() && noexcept
  {
    try {
      done->set_value();
    } catch (...) {
    }
  }

  void set_error(std::exception_ptr ep) && noexcept
  {
    try {
      done->set_exception(ep);
    } catch (...) {
    }
  }

  void set_stopped() && noexcept
  {
    try {
      done->set_exception(std::make_exception_ptr(std::runtime_error("stopped")));
    } catch (...) {
    }
  }
};

// Receiver counting a task's completion kind (value / error / stopped).
struct TaskCountReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::atomic<int>* values;
  std::atomic<int>* errors;
  std::atomic<int>* stopped;

  void set_value() && noexcept
  {
    values->fetch_add(1, std::memory_order_relaxed);
  }

  void set_error(std::exception_ptr) && noexcept
  {
    errors->fetch_add(1, std::memory_order_relaxed);
  }

  void set_stopped() && noexcept
  {
    stopped->fetch_add(1, std::memory_order_relaxed);
  }
};

// Free coroutine functions, not lambdas: everything the body touches is a
// parameter copied/moved into the coroutine frame. A coroutine lambda's
// captures dangle if the closure dies before the lazy task is started.
stdexec::task<void> produce_values(co::mpsc::BoundedSender<int> tx, int count)
{
  for (int i = 1; i <= count; ++i) {
    if (!co_await tx.send(i)) {
      co_return;  // channel closed mid-production
    }
  }
  tx.close();
}

stdexec::task<void> collect_all(co::mpsc::Receiver<int> rx, std::vector<int>* out)
{
  while (auto v = co_await rx.recv()) {
    out->push_back(*v);
  }
}

stdexec::task<void> collect_mapped(co::mpsc::Receiver<int> rx, std::vector<int>* out)
{
  *out = co_await std::move(rx)
             .map([](int x) { return x * 2; })
             .filter([](int x) { return x > 4; })
             .take(2)
             .collect();
}

stdexec::task<void> consume_one(co::mpsc::Receiver<int>* rx, std::atomic<int>* resumed)
{
  (void)co_await rx->recv();
  resumed->fetch_add(1, std::memory_order_relaxed);  // must not run on stop
}

}  // namespace

TEST_CASE("mpsc coroutine producer and consumer (stdexec::task)")
{
  auto [tx, rx] = co::mpsc::bounded<int>(2);
  std::vector<int> collected;

  std::promise<void> done;
  auto fut = done.get_future();
  auto op = stdexec::connect(
      stdexec::starts_on(
          stdexec::inline_scheduler{},
          stdexec::when_all(produce_values(std::move(tx), 5),
                            collect_all(std::move(rx), &collected))),
      VoidPromiseReceiver{&done});
  stdexec::start(op);  // both coroutines interleave inline on this thread:
                       // park, claim, and wake all fire through the channel

  REQUIRE(fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  fut.get();  // rethrows a task error, if any
  CHECK(collected == std::vector<int>{1, 2, 3, 4, 5});
}

TEST_CASE("mpsc receiver stream combinators in a coroutine")
{
  auto [tx, rx] = co::mpsc::unbounded<int>();
  CHECK(tx.send(1));
  CHECK(tx.send(2));

  std::vector<int> collected;
  std::promise<void> done;
  auto fut = done.get_future();
  auto op = stdexec::connect(
      stdexec::starts_on(stdexec::inline_scheduler{},
                         collect_mapped(std::move(rx), &collected)),
      VoidPromiseReceiver{&done});
  stdexec::start(op);
  // Consumed the buffered 1, 2 (both filtered out: 2, 4 are not > 4), then
  // parked on the empty channel inside take(2).
  CHECK(fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);

  CHECK(tx.send(3));  // direct hand-off resumes the coroutine: 6 passes
  CHECK(fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready);
  CHECK(tx.send(4));  // 8 passes; take(2) reached -> collect() completes
  REQUIRE(fut.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
  fut.get();
  CHECK(collected == std::vector<int>{6, 8});
}

TEST_CASE("stdexec::task co_await recv cancelled via stop token")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  stdexec::inplace_stop_source src;
  std::atomic<int> resumed{0}, values{0}, errors{0}, stopped{0};

  {
    auto op = stdexec::connect(
        stdexec::starts_on(
            stdexec::inline_scheduler{},
            stdexec::write_env(
                consume_one(&rx, &resumed),
                stdexec::prop{stdexec::get_stop_token, src.get_token()})),
        TaskCountReceiver{&values, &errors, &stopped});
    stdexec::start(op);  // recv parks on the empty channel
    src.request_stop();

    // The task propagates the channel's set_stopped without resuming the
    // coroutine (same verified behavior as the send side).
    CHECK(stopped.load(std::memory_order_relaxed) == 1);
    CHECK(resumed.load(std::memory_order_relaxed) == 0);
    CHECK(values.load(std::memory_order_relaxed) == 0);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
  }  // opstate destroyed before the stop source

  // The cancelled recv released the slot: the channel keeps working.
  CHECK(sync_send_bounded(tx, 5));
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 5);
  CHECK(!timed_out);
  tx.close();
  CHECK(recv_expect_closed(rx));
}

namespace {

// Producer coroutine that records how many sends were accepted before a
// cancellation hits.
stdexec::task<void> produce_counted(co::mpsc::BoundedSender<int> tx, int count,
                                 std::atomic<int>* accepted)
{
  for (int i = 1; i <= count; ++i) {
    if (!co_await tx.send(i)) {
      co_return;  // channel closed mid-production
    }
    accepted->fetch_add(1, std::memory_order_relaxed);
  }
  tx.close();
}

}  // namespace

TEST_CASE("mpsc coroutine producer cancelled mid-production")
{
  auto [tx, rx] = co::mpsc::bounded<int>(1);
  stdexec::inplace_stop_source src;
  std::atomic<int> accepted{0}, values{0}, errors{0}, stopped{0};

  {
    auto op = stdexec::connect(
        stdexec::starts_on(
            stdexec::inline_scheduler{},
            stdexec::write_env(
                produce_counted(tx, 5, &accepted),
                stdexec::prop{stdexec::get_stop_token, src.get_token()})),
        TaskCountReceiver{&values, &errors, &stopped});
    stdexec::start(op);
    // send(1) was accepted into the buffer; send(2) parked (channel full).
    CHECK(accepted.load(std::memory_order_relaxed) == 1);
    src.request_stop();

    // The parked send is cancelled: the coroutine never resumes (no further
    // sends happen), the task completes stopped, and value 2 is withdrawn.
    CHECK(stopped.load(std::memory_order_relaxed) == 1);
    CHECK(accepted.load(std::memory_order_relaxed) == 1);
    CHECK(values.load(std::memory_order_relaxed) == 0);
    CHECK(errors.load(std::memory_order_relaxed) == 0);
  }  // opstate destroyed before the stop source

  // Only 1 ever entered the channel; 2 must not appear.
  bool timed_out = false;
  CHECK(recv_value(rx, std::chrono::seconds(1), timed_out) == 1);
  CHECK(!timed_out);
  tx.close();
  CHECK(recv_expect_closed(rx));
}
