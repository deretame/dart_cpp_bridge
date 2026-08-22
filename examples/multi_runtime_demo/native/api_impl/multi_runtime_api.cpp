// multi_runtime_api.cpp — business implementation for the stdexec
// multi-runtime demo.
//
// The main bridge runtime talks to two independent WorkerRuntime instances.
// Worker tasks are named stdexec::task functions with state passed as
// parameters; no coroutine lambda captures are used.

#include "multi_runtime_api.h"

#include "../worker_runtime.hpp"

#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/stream_sink.hpp"

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace multi_rt::api {

namespace {

std::mutex g_mu;
std::unique_ptr<WorkerRuntime> g_worker_a;
std::unique_ptr<WorkerRuntime> g_worker_b;
std::atomic<std::uint32_t> g_ping_counter{0};

using StringTx = co::oneshot::Sender<std::string>;
using StringRx = co::oneshot::Receiver<std::string>;
using MpscTx = co::mpsc::Sender<std::string>;
using MpscRx = co::mpsc::Receiver<std::string>;

template <stdexec::sender S>
void start_main_detached(S&& sndr) {
  exec::start_detached(
      stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                         std::forward<S>(sndr))
      | stdexec::upon_error([](std::exception_ptr ep) noexcept {
          try {
            std::rethrow_exception(ep);
          } catch (const std::exception& e) {
            std::fprintf(stderr, "[multi_runtime_demo] detached error: %s\n",
                         e.what());
          } catch (...) {
            std::fprintf(stderr, "[multi_runtime_demo] detached error: unknown\n");
          }
        })
      | stdexec::upon_stopped([]() noexcept {
          std::fprintf(stderr, "[multi_runtime_demo] detached task stopped\n");
        }));
}

stdexec::task<void> process_on_worker(StringTx tx, std::string message) {
  tx.send("[A:" + message + "]");
  co_return;
}

stdexec::task<void> ping_on_worker(StringTx tx, std::string payload,
                                   std::uint32_t id) {
  tx.send("[B#" + std::to_string(id) + ":" + payload + "]");
  co_return;
}

stdexec::task<void> pipeline_stage_a(StringTx tx, std::string message) {
  tx.send("A{" + message + "}");
  co_return;
}

stdexec::task<void> pipeline_stage_b(StringRx rx, StringTx tx) {
  auto from_a = co_await std::move(rx);
  if (!from_a) {
    tx.send("pipeline: A dropped");
    co_return;
  }
  tx.send("B[" + *from_a + "]");
  co_return;
}

stdexec::task<void> fanout_on_worker(StringTx tx, std::string message,
                                     std::string prefix) {
  tx.send(std::move(prefix) + ":" + message);
  co_return;
}

stdexec::task<void> produce_worker_stream(dcb::IoContextScheduler scheduler,
                                          MpscTx tx, std::int32_t count,
                                          std::int32_t interval_ms) {
  for (std::int32_t i = 0; i < count; ++i) {
    if (interval_ms > 0) {
      co_await scheduler.schedule_after(std::chrono::milliseconds(interval_ms));
    }
    if (!tx.send("item_" + std::to_string(i))) break;
  }
  co_return;
}

stdexec::task<void> consume_worker_stream(dcb::StreamSink<std::string> sink,
                                          MpscRx rx) {
  while (true) {
    auto item = co_await rx.recv();
    if (!item) break;
    sink.add(*item);
  }
  sink.end();
  co_return;
}

stdexec::task<void> call_dart_on_worker(StringTx tx,
                                        dcb::DartFn<std::string(std::string)> callback,
                                        std::string input) {
  try {
    auto result = co_await callback(input);
    tx.send(std::move(result));
  } catch (const std::exception& e) {
    tx.send(std::string("ERROR: ") + e.what());
  } catch (...) {
    tx.send("ERROR: unknown");
  }
  co_return;
}

}  // namespace

stdexec::task<std::string> start_workers() {
  std::lock_guard lock(g_mu);
  if (!g_worker_a) {
    g_worker_a = std::make_unique<WorkerRuntime>("processor");
    g_worker_a->start();
  }
  if (!g_worker_b) {
    g_worker_b = std::make_unique<WorkerRuntime>("responder");
    g_worker_b->start();
  }
  co_return "workers started";
}

stdexec::task<std::string> stop_workers() {
  std::unique_ptr<WorkerRuntime> worker_a;
  std::unique_ptr<WorkerRuntime> worker_b;
  {
    std::lock_guard lock(g_mu);
    worker_a = std::move(g_worker_a);
    worker_b = std::move(g_worker_b);
  }

  // Joining a worker can wait for a task that is awaiting a Dart reply. Do
  // that on the blocking pool so the main bridge io thread remains able to
  // deliver the reply and resume the worker task.
  co_await dcb::spawn_blocking(
      [worker_a = std::move(worker_a), worker_b = std::move(worker_b)]() mutable {
        if (worker_a) worker_a->stop();
        if (worker_b) worker_b->stop();
      });
  co_return "workers stopped";
}

stdexec::task<std::string> process_message(std::string message) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_worker_a || !g_worker_a->running()) {
      throw std::runtime_error("worker A not running");
    }
    g_worker_a->spawn(process_on_worker(std::move(tx), std::move(message)));
  }
  auto reply = co_await std::move(rx);
  if (!reply) throw std::runtime_error("worker A dropped");
  co_return *reply;
}

stdexec::task<std::string> ping_worker(std::string payload) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  const auto id = g_ping_counter.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(g_mu);
    if (!g_worker_b || !g_worker_b->running()) {
      throw std::runtime_error("worker B not running");
    }
    g_worker_b->spawn(ping_on_worker(std::move(tx), std::move(payload), id));
  }
  auto reply = co_await std::move(rx);
  if (!reply) throw std::runtime_error("worker B dropped");
  co_return *reply;
}

stdexec::task<std::string> pipeline(std::string message) {
  auto [tx_final, rx_final] = co::oneshot::channel<std::string>();
  auto [tx_ab, rx_ab] = co::oneshot::channel<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_worker_a || !g_worker_a->running() ||
        !g_worker_b || !g_worker_b->running()) {
      throw std::runtime_error("workers not running");
    }
    g_worker_a->spawn(pipeline_stage_a(std::move(tx_ab), std::move(message)));
    g_worker_b->spawn(pipeline_stage_b(std::move(rx_ab), std::move(tx_final)));
  }
  auto reply = co_await std::move(rx_final);
  co_return reply.value_or("pipeline: lost");
}

stdexec::task<std::pair<std::string, std::string>> fan_out(std::string message) {
  auto [tx_a, rx_a] = co::oneshot::channel<std::string>();
  auto [tx_b, rx_b] = co::oneshot::channel<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_worker_a || !g_worker_a->running() ||
        !g_worker_b || !g_worker_b->running()) {
      throw std::runtime_error("workers not running");
    }
    g_worker_a->spawn(fanout_on_worker(std::move(tx_a), message, "A"));
    g_worker_b->spawn(fanout_on_worker(std::move(tx_b), std::move(message), "B"));
  }
  auto a = co_await std::move(rx_a);
  auto b = co_await std::move(rx_b);
  co_return std::make_pair(a.value_or("A:lost"), b.value_or("B:lost"));
}

void worker_stream(dcb::StreamSink<std::string> sink, std::int32_t count,
                   std::int32_t interval_ms) {
  auto [tx, rx] = co::mpsc::unbounded<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_worker_a || !g_worker_a->running()) {
      sink.error("worker A not running");
      return;
    }
    g_worker_a->spawn(produce_worker_stream(g_worker_a->scheduler(),
                                            std::move(tx), count, interval_ms));
  }
  start_main_detached(consume_worker_stream(std::move(sink), std::move(rx)));
}

stdexec::task<std::string> call_dart_from_worker_a(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_worker_a || !g_worker_a->running()) {
      throw std::runtime_error("worker A not running");
    }
    g_worker_a->spawn(call_dart_on_worker(std::move(tx), std::move(callback),
                                          std::move(input)));
  }
  auto reply = co_await std::move(rx);
  if (!reply) throw std::runtime_error("worker A dropped");
  co_return *reply;
}

stdexec::task<std::string> call_dart_from_worker_b(
    dcb::DartFn<std::string(std::string)> callback, std::string input) {
  auto [tx, rx] = co::oneshot::channel<std::string>();
  {
    std::lock_guard lock(g_mu);
    if (!g_worker_b || !g_worker_b->running()) {
      throw std::runtime_error("worker B not running");
    }
    g_worker_b->spawn(call_dart_on_worker(std::move(tx), std::move(callback),
                                          std::move(input)));
  }
  auto reply = co_await std::move(rx);
  if (!reply) throw std::runtime_error("worker B dropped");
  co_return *reply;
}

}  // namespace multi_rt::api
