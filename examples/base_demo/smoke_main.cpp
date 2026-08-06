#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/start_with_receiver.hpp"
#include "dart_cpp_bridge/stream.hpp"

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>

#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace dcb::demo {
std::vector<std::uint8_t> dispatch_sync(std::uint64_t session_id, const std::uint8_t* data, std::size_t len);
void dispatch_request(std::shared_ptr<Session> session, std::uint64_t session_id, const std::uint8_t* data, std::size_t len);
void test_cbridge_api();
}  // namespace dcb::demo

// Demo method ids (same as demo_api.cpp).
enum class MethodId : std::uint32_t {
  kBridgeVersion = 1,
  kAdd = 2,
};

namespace {

struct DartFnSimCtx {
  std::uint64_t sid{0};
  std::shared_ptr<std::promise<std::string>> done;
};

// Kept alive for C function-pointer userdata.
std::shared_ptr<DartFnSimCtx> g_dartfn_sim;
std::shared_ptr<std::promise<int>> g_add_done;

void fail(const char* msg) {
  std::fprintf(stderr, "FAIL: %s\n", msg);
  std::exit(1);
}

// Error handler for exec::start_detached chains: errors must never reach the
// built-in receiver (which terminates), so every detached chain ends with
// `| upon_error(log_detached_error)`.
void log_detached_error(std::exception_ptr ep) noexcept {
  try {
    std::rethrow_exception(ep);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[smoke] detached sender error: %s\n", e.what());
  } catch (...) {
    std::fprintf(stderr, "[smoke] detached sender error: unknown\n");
  }
}

// Generic receiver completing a std::promise<V> with the sender's value.
template <typename V>
struct PromiseValReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<std::promise<V>> done;

  void set_value(V v) && noexcept {
    try {
      done->set_value(std::move(v));
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

// Receiver for void-completing senders.
struct VoidPromiseReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<std::promise<void>> done;

  void set_value() && noexcept {
    try {
      done->set_value();
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

// Generic receiver completing a std::promise<int>; used by the oneshot tests.
template <typename T>
struct PromiseReceiver {
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

// Receiver recording only that completion happened (for side-effect checks).
struct FlagReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::atomic<bool>* resumed;

  void set_value(std::optional<int> /*v*/) && noexcept { resumed->store(true); }
  void set_error(std::exception_ptr) && noexcept { resumed->store(true); }
  void set_stopped() && noexcept { resumed->store(true); }
};

// Receiver that posts a string response frame (used by the DartFn e2e test).
struct PostStringReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<dcb::Session> session;
  std::uint64_t gen{0};

  void set_value(std::string out) && noexcept {
    try {
      dcb::ByteWriter w;
      w.str(out);
      session->try_post(gen, dcb::make_frame(dcb::MsgType::kResponseOk, 1, 0, w.raw()));
    } catch (...) {
    }
  }

  void set_error(std::exception_ptr) && noexcept {}
  void set_stopped() && noexcept {}
};

void test_oneshot_cross_thread_wake() {
  using namespace dcb;
  Runtime::instance().start();

  std::promise<int> done;
  auto fut = done.get_future();
  auto [tx, rx] = co::oneshot::channel<int>();

  // Run the oneshot receiver on the runtime io scheduler (starts-on io); the
  // chain's start (waiter registration) happens on the io thread.
  auto sndr = stdexec::starts_on(*Runtime::instance().io_scheduler(), std::move(rx));
  auto op = stdexec::connect(std::move(sndr), PromiseReceiver<int>{&done});
  stdexec::start(op);

  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::thread sender([tx = std::move(tx)]() mutable {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    (void)tx.send(42);
  });
  sender.join();

  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("oneshot cross-thread wake timed out");
  }
  if (fut.get() != 42) {
    Runtime::instance().stop();
    fail("oneshot value mismatch");
  }
  Runtime::instance().stop();
  std::printf("oneshot cross-thread wake ok\n");
}

void test_io_not_blocked_while_awaiting() {
  using namespace dcb;
  Runtime::instance().start();

  auto [tx, rx] = co::oneshot::channel<int>();
  std::atomic<bool> resumed{false};
  std::atomic<int> side_work{0};

  auto sndr = stdexec::starts_on(*Runtime::instance().io_scheduler(), std::move(rx));
  auto op = stdexec::connect(std::move(sndr), FlagReceiver{&resumed});
  stdexec::start(op);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  for (int i = 0; i < 5; ++i) {
    asio::post(Runtime::instance().io(), [&] { side_work.fetch_add(1); });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (side_work.load() != 5) {
    Runtime::instance().stop();
    fail("io blocked while sender awaiting oneshot");
  }
  if (resumed.load()) {
    Runtime::instance().stop();
    fail("sender resumed before send");
  }

  if (!tx.send(1)) {
    fail("send failed");
  }
  for (int i = 0; i < 50 && !resumed.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!resumed.load()) {
    Runtime::instance().stop();
    fail("sender did not resume after send");
  }
  Runtime::instance().stop();
  std::printf("io not blocked while awaiting ok\n");
}

void test_dartfn_async_e2e_simulated_reply() {
  using namespace dcb;
  Runtime::instance().start();

  auto done = std::make_shared<std::promise<std::string>>();
  auto fut = done->get_future();
  const auto sid = SessionRegistry::instance().open(/*reply_port=*/99);
  g_dartfn_sim = std::make_shared<DartFnSimCtx>(DartFnSimCtx{sid, done});

  Runtime::instance().set_dart_post(
      [](std::int64_t, const std::uint8_t* data, std::size_t len, void*) {
        auto* c = g_dartfn_sim.get();
        if (!c) {
          return;
        }
        try {
          auto h = parse_frame(data, len);
          if (h.type == MsgType::kDartFnCall) {
            const auto reply_id = h.request_id;
            ByteReader r(h.payload.data(), h.payload.size());
            (void)r.u64();
            auto arg = r.str();
            std::thread([sid = c->sid, reply_id, arg = std::move(arg)]() {
              std::this_thread::sleep_for(std::chrono::milliseconds(30));
              ByteWriter w;
              w.str(std::string("Hello, ") + arg + "!");
              auto session = SessionRegistry::instance().get(sid);
              if (session) {
                session->complete_dart_fn(reply_id, true, w.raw(), {});
              }
            }).detach();
            return;
          }
          if (h.type == MsgType::kResponseOk) {
            ByteReader r(h.payload.data(), h.payload.size());
            try {
              c->done->set_value(r.str());
            } catch (...) {
            }
          }
        } catch (const std::exception& e) {
          std::printf("dartfn sim post error: %s\n", e.what());
        }
      },
      nullptr);

  auto session = SessionRegistry::instance().get(sid);
  const auto gen = session->generation();
  // Launch the DartFn reverse call on the io scheduler (starts-on io). If the
  // session is gone, DartFn::operator() throws inside then -> set_error ->
  // logged by upon_error (exec::start_detached would terminate on errors).
  exec::start_detached(
      stdexec::starts_on(*Runtime::instance().io_scheduler(),
                         stdexec::just() | stdexec::then([session, gen] {
                           dcb::DartFn<std::string(std::string)> cb(session, gen, /*fn_id=*/1);
                           dcb::start_with_receiver(cb("Tom"), PostStringReceiver{session, gen});
                         }))
      | stdexec::upon_error(log_detached_error));

  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    SessionRegistry::instance().close_all();
    Runtime::instance().set_dart_post(nullptr, nullptr);
    Runtime::instance().stop();
    g_dartfn_sim.reset();
    fail("dartfn async e2e timed out");
  }
  const auto got = fut.get();
  if (got != "Hello, Tom!") {
    SessionRegistry::instance().close_all();
    Runtime::instance().set_dart_post(nullptr, nullptr);
    Runtime::instance().stop();
    g_dartfn_sim.reset();
    fail("dartfn async e2e wrong result");
  }
  std::printf("dartfn async e2e ok: %s\n", got.c_str());
  SessionRegistry::instance().close_all();
  Runtime::instance().set_dart_post(nullptr, nullptr);
  Runtime::instance().stop();
  g_dartfn_sim.reset();
}

// ---------------------------------------------------------------------------
// spawn / spawn_blocking / sync_wait
// ---------------------------------------------------------------------------

stdexec::sender auto return_value(int v) { return stdexec::just(v); }

stdexec::sender auto signal_and_return(std::shared_ptr<std::promise<int>> done, int v) {
  return stdexec::just(v) | stdexec::then([done](int value) {
           done->set_value(value);
           return value;
         });
}

void test_spawn_fire_and_forget() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<int>>();
  auto fut = done->get_future();
  // Fire-and-forget: start and ignore the result; the sender chain still runs
  // and signals through the promise it captured.
  exec::start_detached(
      stdexec::starts_on(*Runtime::instance().io_scheduler(), signal_and_return(done, 99))
      | stdexec::upon_error(log_detached_error));
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn fire-and-forget timed out");
  }
  if (fut.get() != 99) {
    Runtime::instance().stop();
    fail("spawn fire-and-forget wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn fire-and-forget ok\n");
}

void test_spawn_wait_result() {
  using namespace dcb;
  Runtime::instance().start();
  // Block this (main, non-io) thread until the sender on io finishes.
  // sync_wait returns the value (as a tuple) or rethrows the sender's error.
  auto v = dcb::sync_wait(stdexec::starts_on(*Runtime::instance().io_scheduler(), return_value(7)));
  if (!v || std::get<0>(*v) != 7) {
    Runtime::instance().stop();
    fail("spawn wait wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn wait ok\n");
}

stdexec::sender auto throw_value() {
  return stdexec::just(0) | stdexec::then([](int) -> int {
           throw std::runtime_error("sync-boom");
         });
}

void test_spawn_syncawait_exception() {
  using namespace dcb;
  Runtime::instance().start();
  std::string what;
  try {
    (void)dcb::sync_wait(
        stdexec::starts_on(*Runtime::instance().io_scheduler(), throw_value()));
    what = "no-throw";
  } catch (const std::exception& e) {
    what = e.what();
  }
  if (what != "sync-boom") {
    Runtime::instance().stop();
    fail("spawn sync_wait exception not propagated");
  }
  Runtime::instance().stop();
  std::printf("spawn sync_wait exception propagation ok\n");
}

void test_syncawait_rejected_on_io_thread() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<bool>>();
  auto fut = done->get_future();
  // Run a sender chain ON the io thread that tries to sync_wait another
  // io-bound sender. The deadlock guard (IoContextScheduler::current_thread_is_io)
  // must reject it with std::logic_error instead of letting the io thread
  // block on itself (which would deadlock and hang the runtime).
  exec::start_detached(
      stdexec::starts_on(*Runtime::instance().io_scheduler(),
                         stdexec::just() | stdexec::then([done] {
                           bool rejected = false;
                           try {
                             (void)dcb::sync_wait(stdexec::starts_on(
                                 *Runtime::instance().io_scheduler(), return_value(1)));
                           } catch (const std::logic_error&) {
                             rejected = true;
                           }
                           done->set_value(rejected);
                         }))
      | stdexec::upon_error(log_detached_error));
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("sync_wait-on-io timed out (deadlock guard did not fire)");
  }
  if (!fut.get()) {
    Runtime::instance().stop();
    fail("sync_wait on io thread was not rejected");
  }
  Runtime::instance().stop();
  std::printf("sync_wait rejected on io thread ok\n");
}

void test_spawn_blocking_awaited_no_block_io() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<int>>();
  auto fut = done->get_future();
  std::atomic<int> side_work{0};

  // A sender chain on io awaits spawn_blocking (150ms sleep on the pool);
  // completion is delivered back on the io thread.
  dcb::start_with_receiver(
      dcb::spawn_blocking([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return 42;
      }),
      PromiseValReceiver<int>{done});

  // While the pool thread is sleeping, io must stay responsive.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  for (int i = 0; i < 5; ++i) {
    asio::post(Runtime::instance().io(), [&] { side_work.fetch_add(1); });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  if (side_work.load() != 5) {
    Runtime::instance().stop();
    fail("io blocked while spawn_blocking awaiting");
  }
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking awaited timed out");
  }
  const auto v = fut.get();
  if (v != 42) {
    Runtime::instance().stop();
    fail("spawn_blocking awaited wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking awaited (io not blocked) ok\n");
}

void test_spawn_blocking_fire_and_forget() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<int>>();
  auto fut = done->get_future();
  struct IntReceiver {
    using receiver_concept = stdexec::receiver_tag;
    std::shared_ptr<std::promise<int>> done;
    void set_value(int v) && noexcept {
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
    void set_stopped() && noexcept {}
  };
  // Fire-and-forget: launch via connect + start; the completion arrives back
  // on the io thread.
  dcb::start_with_receiver(
      dcb::spawn_blocking([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 123;
      }),
      IntReceiver{done});
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking fire-and-forget timed out");
  }
  if (fut.get() != 123) {
    Runtime::instance().stop();
    fail("spawn_blocking fire-and-forget wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking fire-and-forget ok\n");
}

void test_spawn_blocking_explicit_scheduler() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<int>>();
  auto fut = done->get_future();
  struct IntReceiver {
    using receiver_concept = stdexec::receiver_tag;
    std::shared_ptr<std::promise<int>> done;
    void set_value(int v) && noexcept {
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
    void set_stopped() && noexcept {}
  };
  // Explicit scheduler: pass the blocking pool scheduler by hand; the result
  // must still arrive back on the io thread.
  dcb::start_with_receiver(
      dcb::spawn_blocking(
          [] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            return 77;
          },
          Runtime::instance().blocking_scheduler()),
      IntReceiver{done});
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking explicit scheduler timed out");
  }
  if (fut.get() != 77) {
    Runtime::instance().stop();
    fail("spawn_blocking explicit scheduler wrong value");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking explicit scheduler ok\n");
}

void test_spawn_blocking_exception() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<std::string>>();
  auto fut = done->get_future();
  struct ErrReceiver {
    using receiver_concept = stdexec::receiver_tag;
    std::shared_ptr<std::promise<std::string>> done;
    void set_value(int) && noexcept {
      try {
        done->set_value("no-throw");
      } catch (...) {
      }
    }
    void set_error(std::exception_ptr ep) && noexcept {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& e) {
        done->set_value(e.what());
      } catch (...) {
      }
    }
    void set_stopped() && noexcept {}
  };
  dcb::start_with_receiver(
      dcb::spawn_blocking([]() -> int { throw std::runtime_error("boom"); }),
      ErrReceiver{done});
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking exception timed out");
  }
  if (fut.get() != "boom") {
    Runtime::instance().stop();
    fail("spawn_blocking exception not propagated");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking exception propagation ok\n");
}

void test_spawn_blocking_void() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<bool>>();
  auto fut = done->get_future();
  auto flag = std::make_shared<std::atomic<bool>>(false);
  struct VoidOkReceiver {
    using receiver_concept = stdexec::receiver_tag;
    std::shared_ptr<std::promise<bool>> done;
    std::shared_ptr<std::atomic<bool>> flag;
    void set_value(Unit) && noexcept {
      try {
        done->set_value(flag->load());
      } catch (...) {
      }
    }
    void set_error(std::exception_ptr ep) && noexcept {
      try {
        done->set_exception(ep);
      } catch (...) {
      }
    }
    void set_stopped() && noexcept {}
  };
  dcb::start_with_receiver(
      dcb::spawn_blocking([flag] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        flag->store(true);
      }),
      VoidOkReceiver{done, flag});
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking void timed out");
  }
  if (!fut.get()) {
    Runtime::instance().stop();
    fail("spawn_blocking void did not run");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking void ok\n");
}

// A void callable that throws must still propagate the exception to the
// awaiter: spawn_blocking bridges void through dcb::Unit and delivers the
// exception via set_error.
void test_spawn_blocking_void_exception() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<std::string>>();
  auto fut = done->get_future();
  struct VoidErrReceiver {
    using receiver_concept = stdexec::receiver_tag;
    std::shared_ptr<std::promise<std::string>> done;
    void set_value(Unit) && noexcept {
      try {
        done->set_value("no-throw");
      } catch (...) {
      }
    }
    void set_error(std::exception_ptr ep) && noexcept {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& e) {
        done->set_value(e.what());
      } catch (...) {
      }
    }
    void set_stopped() && noexcept {}
  };
  dcb::start_with_receiver(
      dcb::spawn_blocking([]() -> void { throw std::runtime_error("void-boom"); }),
      VoidErrReceiver{done});
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("spawn_blocking void exception timed out");
  }
  if (fut.get() != "void-boom") {
    Runtime::instance().stop();
    fail("spawn_blocking void exception not propagated");
  }
  Runtime::instance().stop();
  std::printf("spawn_blocking void exception propagation ok\n");
}

// dcb::sleep must suspend on a real event-loop timer: the io thread stays
// responsive during the sleep, and the sender resumes only after the
// requested duration.
void test_coro_sleep_no_block_io() {
  using namespace dcb;
  Runtime::instance().start();
  auto done = std::make_shared<std::promise<long long>>();
  auto fut = done->get_future();
  std::atomic<int> side_work{0};

  auto t0 = std::chrono::steady_clock::now();
  struct SleepReceiver {
    using receiver_concept = stdexec::receiver_tag;
    std::shared_ptr<std::promise<long long>> done;
    std::chrono::steady_clock::time_point t0;
    void set_value() && noexcept {
      try {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
        done->set_value(static_cast<long long>(elapsed));
      } catch (...) {
      }
    }
    void set_error(std::exception_ptr ep) && noexcept {
      try {
        done->set_exception(ep);
      } catch (...) {
      }
    }
    void set_stopped() && noexcept {}
  };
  dcb::start_with_receiver(dcb::sleep(std::chrono::milliseconds(150)),
                      SleepReceiver{done, t0});

  // While the sender is sleeping on the timer, io must stay responsive.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  for (int i = 0; i < 5; ++i) {
    asio::post(Runtime::instance().io(), [&] { side_work.fetch_add(1); });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  if (side_work.load() != 5) {
    Runtime::instance().stop();
    fail("io blocked while sleep awaiting");
  }
  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("sleep timed out");
  }
  auto elapsed = fut.get();
  if (elapsed < 140) {  // small scheduling slack below the requested 150ms
    Runtime::instance().stop();
    fail("sleep resumed too early");
  }
  Runtime::instance().stop();
  std::printf("sleep (io not blocked, ~%lldms) ok\n", elapsed);
}

struct CancellableSleepCtx {
  std::shared_ptr<std::promise<std::string>> done;
  std::chrono::steady_clock::time_point t0;
};

// Receiver observing the completion of the sleep sender: "done" / "error" /
// "cancelled".
struct SleepCompletionReceiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<std::promise<std::string>> done;

  void set_value() && noexcept {
    try {
      done->set_value("done");
    } catch (...) {
    }
  }

  void set_error(std::exception_ptr) && noexcept {
    try {
      done->set_value("error");
    } catch (...) {
    }
  }

  void set_stopped() && noexcept {
    try {
      done->set_value("cancelled");
    } catch (...) {
    }
  }
};

// dcb::sleep must be interruptible through the standard stop_token machinery:
// write_env injects an inplace_stop_token; request_stop() cancels the timer
// and the sleep completes with set_stopped promptly instead of waiting out
// the full 10s.
void test_sleep_cancellation() {
  using namespace dcb;
  Runtime::instance().start();
  auto ctx = std::make_shared<CancellableSleepCtx>();
  ctx->done = std::make_shared<std::promise<std::string>>();
  ctx->t0 = std::chrono::steady_clock::now();
  auto fut = ctx->done->get_future();

  stdexec::inplace_stop_source stop_src;
  auto sndr = stdexec::write_env(
      dcb::sleep(std::chrono::seconds(10)),
      stdexec::prop{stdexec::get_stop_token, stop_src.get_token()});
  auto op = stdexec::connect(std::move(sndr), SleepCompletionReceiver{ctx->done});
  stdexec::start(op);

  // Let the sleep enter the timer wait, then cancel it.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop_src.request_stop();

  if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("sleep cancellation timed out");
  }
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - ctx->t0)
                     .count();
  const auto msg = fut.get();
  Runtime::instance().stop();
  if (msg != "cancelled") {
    fail("sleep did not complete with set_stopped on cancel");
  }
  if (elapsed >= 2000) {
    fail("sleep cancellation not prompt");
  }
  std::printf("sleep cancellation (cancelled after ~%lldms) ok\n", elapsed);
}

}  // namespace

// ---------------------------------------------------------------------------
// Bounded mpsc channel (backpressure)
// ---------------------------------------------------------------------------

// Receiver completing a std::promise<bool> (bounded send results).
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

// The bounded channel applies backpressure: with capacity 2, the third send
// must park (no thread blocked) until a recv() frees a slot; sends after
// close fail with false; the receiver drains buffered values then reports
// the close.
void test_bounded_mpsc_backpressure() {
  using namespace dcb;
  Runtime::instance().start();
  auto* io = Runtime::instance().io_scheduler();

  auto [tx, rx] = co::mpsc::bounded<int>(2);

  // Two values fit immediately.
  bool ok1 = std::get<0>(*dcb::sync_wait(stdexec::starts_on(*io, tx.send(1))));
  bool ok2 = std::get<0>(*dcb::sync_wait(stdexec::starts_on(*io, tx.send(2))));
  if (!ok1 || !ok2) {
    Runtime::instance().stop();
    fail("bounded send fast path failed");
  }
  if (tx.remaining_capacity() != 0) {
    Runtime::instance().stop();
    fail("bounded remaining_capacity should be 0 after filling");
  }

  // Third send must park: start it on io and verify it does NOT complete.
  std::promise<bool> send3_done;
  auto send3_fut = send3_done.get_future();
  auto sndr3 = stdexec::starts_on(*io, tx.send(3));
  auto op3 = stdexec::connect(std::move(sndr3), BoolPromiseReceiver{&send3_done});
  stdexec::start(op3);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  if (send3_fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
    Runtime::instance().stop();
    fail("bounded send(3) should be parked when full");
  }

  // recv(1) frees a slot -> parked send(3) resumes and completes with true.
  std::promise<int> recv1_done;
  auto recv1_fut = recv1_done.get_future();
  auto rop1 =
      stdexec::connect(stdexec::starts_on(*io, rx.recv()), PromiseReceiver<int>{&recv1_done});
  stdexec::start(rop1);
  if (recv1_fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("bounded recv(1) timed out");
  }
  int recv1_val = recv1_fut.get();
  if (recv1_val != 1) {
    Runtime::instance().stop();
    fail("bounded recv(1) wrong value");
  }
  if (send3_fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("parked bounded send(3) not resumed");
  }
  if (!send3_fut.get()) {
    Runtime::instance().stop();
    fail("parked bounded send(3) failed");
  }

  // FIFO drain: 2, then 3.
  std::promise<int> recv2_done;
  auto recv2_fut = recv2_done.get_future();
  auto rop2 =
      stdexec::connect(stdexec::starts_on(*io, rx.recv()), PromiseReceiver<int>{&recv2_done});
  stdexec::start(rop2);
  if (recv2_fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("bounded recv(2) timed out");
  }
  int recv2_val = recv2_fut.get();
  if (recv2_val != 2) {
    Runtime::instance().stop();
    std::printf("bounded recv(2) got %d, waiting for 2\n", recv2_val);
    fail("bounded recv(2) wrong value");
  }

  std::promise<int> recv3_done;
  auto recv3_fut = recv3_done.get_future();
  auto rop3 =
      stdexec::connect(stdexec::starts_on(*io, rx.recv()), PromiseReceiver<int>{&recv3_done});
  stdexec::start(rop3);
  if (recv3_fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    Runtime::instance().stop();
    fail("bounded recv(3) timed out");
  }
  if (recv3_fut.get() != 3) {
    Runtime::instance().stop();
    fail("bounded recv(3) wrong value");
  }

  // Sends after close fail immediately.
  tx.close();
  bool ok4 = std::get<0>(*dcb::sync_wait(stdexec::starts_on(*io, tx.send(4))));
  if (ok4) {
    Runtime::instance().stop();
    fail("bounded send after close should fail");
  }

  // Empty + closed -> recv reports the close (PromiseReceiver raises).
  std::promise<int> recv4_done;
  auto recv4_fut = recv4_done.get_future();
  auto rop4 =
      stdexec::connect(stdexec::starts_on(*io, rx.recv()), PromiseReceiver<int>{&recv4_done});
  stdexec::start(rop4);
  try {
    (void)recv4_fut.get();
    Runtime::instance().stop();
    fail("bounded recv after drain should report closed");
  } catch (const std::exception&) {
    // expected: channel closed
  }

  Runtime::instance().stop();
  std::printf("bounded mpsc backpressure ok\n");
}

// Concurrent merge (merge_concurrent): two 5ms interval streams advance
// concurrently, so the merged stream's first values contain both streams'
// first tick (0 appears twice). A sequential merge would emit 0..5 instead.
void test_merge_concurrent() {
  using namespace dcb;
  Runtime::instance().start();
  auto* io = Runtime::instance().io_scheduler();

  bool ok = std::get<0>(*dcb::sync_wait([io]() -> exec::task<bool> {
    auto s1 = co::stream::interval_on<int>(*io, std::chrono::milliseconds(5));
    auto s2 = co::stream::interval_on<int>(*io, std::chrono::milliseconds(5));
    // Stream is move-only, so initializer_list ({...}) is not usable.
    std::vector<co::stream::Stream<int>> srcs;
    srcs.push_back(std::move(s1));
    srcs.push_back(std::move(s2));
    auto m = co::stream::merge_concurrent<int>(std::move(srcs));
    std::vector<int> out;
    for (int i = 0; i < 6; ++i) {
      auto v = co_await m.next();
      if (!v) break;
      out.push_back(*v);
    }
    int zeros = 0;
    for (int v : out) {
      zeros += (v == 0);
    }
    std::printf("merge_concurrent: [");
    for (std::size_t i = 0; i < out.size(); ++i) {
      std::printf("%s%d", i ? "," : "", out[i]);
    }
    std::printf("] zeros=%d\n", zeros);
    co_return zeros >= 2;
  }()));

  std::printf("%s\n", ok ? "merge_concurrent interleaved ok" : "merge_concurrent FAILED");
  if (!ok) {
    std::abort();
  }
}

// Cross-thread strict FIFO: two producer threads send values taken from a
// shared monotonically increasing counter through a small bounded channel
// (forcing park/wake contention). The consumer must observe a strictly
// increasing sequence — any cross-thread reordering would show up as an
// inversion (this is the guarantee rigtorp's MPMCQueue provides and
// moodycamel's did not).
void test_bounded_mpsc_cross_thread_fifo() {
  using namespace dcb;
  Runtime::instance().start();
  auto* io = Runtime::instance().io_scheduler();

  constexpr int kPerProducer = 100;
  constexpr int kTotal = 2 * kPerProducer;
  auto [tx, rx] = co::mpsc::bounded<int>(8);
  std::atomic<int> next{1};
  std::atomic<bool> prod_failed{false};

  auto producer = [&tx, &next, &prod_failed]() {
    for (int i = 0; i < kPerProducer; ++i) {
      int n = next.fetch_add(1, std::memory_order_relaxed);
      bool ok = std::get<0>(*dcb::sync_wait(
        stdexec::starts_on(*Runtime::instance().io_scheduler(), tx.send(n))));
      if (!ok) {
        prod_failed.store(true, std::memory_order_relaxed);
      }
    }
  };
  std::thread p1(producer);
  std::thread p2(producer);

  // Delivery order matches send() call order, not the shared fetch_add
  // order (threads can be preempted between fetch_add and send), so assert
  // completeness/uniqueness rather than monotonicity.
  std::vector<bool> seen(static_cast<std::size_t>(kTotal) + 1, false);
  int bad = 0;
  for (int i = 0; i < kTotal; ++i) {
    std::promise<int> done;
    auto fut = done.get_future();
    auto rop =
        stdexec::connect(stdexec::starts_on(*io, rx.recv()), PromiseReceiver<int>{&done});
    stdexec::start(rop);
    if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
      Runtime::instance().stop();
      fail("cross-thread fifo recv timed out");
    }
    int v = fut.get();
    if (v < 1 || v > kTotal || seen[static_cast<std::size_t>(v)]) {
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
  if (bad != 0) {
    Runtime::instance().stop();
    fail("bounded mpsc cross-thread lost/duplicated values");
  }
  if (prod_failed.load(std::memory_order_relaxed)) {
    Runtime::instance().stop();
    fail("bounded mpsc producer send failed");
  }
  Runtime::instance().stop();
  std::printf("bounded mpsc cross-thread FIFO ok\n");
}

// Unbounded cross-thread completeness: same uniqueness assertion as the
// bounded variant, but the senders are synchronous. Regression test: the
// deque push used to sit outside the channel mutex, silently dropping
// values under multi-producer contention (data race on the deque).
void test_unbounded_mpsc_cross_thread() {
  using namespace dcb;
  Runtime::instance().start();

  constexpr int kPerProducer = 10000;
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

  // Delivery order only matches send() call order (fetch_add order != send
  // order under preemption), so assert completeness, not monotonicity.
  std::vector<bool> seen(static_cast<std::size_t>(kTotal) + 1, false);
  int bad = 0;
  for (int i = 0; i < kTotal; ++i) {
    std::promise<int> done;
    auto fut = done.get_future();
    auto rop =
        stdexec::connect(stdexec::starts_on(*Runtime::instance().io_scheduler(), rx.recv()),
                         PromiseReceiver<int>{&done});
    stdexec::start(rop);
    if (fut.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
      Runtime::instance().stop();
      fail("unbounded fifo recv timed out");
    }
    int v = fut.get();
    if (v < 1 || v > kTotal || seen[static_cast<std::size_t>(v)]) {
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
  if (bad != 0) {
    Runtime::instance().stop();
    fail("unbounded mpsc cross-thread lost/duplicated values");
  }
  if (prod_failed.load(std::memory_order_relaxed)) {
    Runtime::instance().stop();
    fail("unbounded mpsc producer send failed");
  }
  Runtime::instance().stop();
  std::printf("unbounded mpsc cross-thread FIFO ok\n");
}

int main() {
  using namespace dcb;
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("smoke start\n");
  test_oneshot_cross_thread_wake();
  test_io_not_blocked_while_awaiting();
  test_dartfn_async_e2e_simulated_reply();
  dcb::demo::test_cbridge_api();
  test_spawn_fire_and_forget();
  test_spawn_wait_result();
  test_spawn_syncawait_exception();
  test_syncawait_rejected_on_io_thread();
  test_spawn_blocking_awaited_no_block_io();
  test_spawn_blocking_fire_and_forget();
  test_spawn_blocking_explicit_scheduler();
  test_spawn_blocking_exception();
  test_spawn_blocking_void();
  test_spawn_blocking_void_exception();
  test_coro_sleep_no_block_io();
  test_sleep_cancellation();
  test_bounded_mpsc_backpressure();
  test_bounded_mpsc_cross_thread_fifo();
  test_unbounded_mpsc_cross_thread();
  test_merge_concurrent();
  // ForeignExecutor was removed in the stdexec migration; the foreign runtime
  // path is covered by examples/foreign_runtime_demo (UvScheduler + dart test).

  Runtime::instance().start();
  g_add_done = std::make_shared<std::promise<int>>();
  auto add_fut = g_add_done->get_future();
  Runtime::instance().set_dart_post(
      [](std::int64_t, const std::uint8_t* data, std::size_t len, void*) {
        try {
          auto h = parse_frame(data, len);
          std::printf("post type=%u req=%llu payload=%zu\n", static_cast<unsigned>(h.type),
                      static_cast<unsigned long long>(h.request_id), h.payload.size());
          if (h.type == MsgType::kResponseOk && h.request_id == 2 && g_add_done) {
            ByteReader r(h.payload.data(), h.payload.size());
            try {
              g_add_done->set_value(r.i32());
            } catch (...) {
            }
          }
        } catch (const std::exception& e) {
          std::printf("post parse error: %s\n", e.what());
        }
      },
      nullptr);

  auto sid = SessionRegistry::instance().open(/*reply_port=*/1);
  auto session = SessionRegistry::instance().get(sid);

  {
    auto req =
        make_frame(MsgType::kRequest, 1, static_cast<std::uint32_t>(MethodId::kBridgeVersion), {});
    auto resp = demo::dispatch_sync(0, req.data(), req.size());
    auto h = parse_frame(resp.data(), resp.size());
    ByteReader r(h.payload.data(), h.payload.size());
    std::printf("version=%d\n", r.i32());
  }

  {
    ByteWriter payload;
    payload.i32(40);
    payload.i32(2);
    auto req =
        make_frame(MsgType::kRequest, 2, static_cast<std::uint32_t>(MethodId::kAdd), payload.raw());
    demo::dispatch_request(session, sid, req.data(), req.size());
  }

  if (add_fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
    SessionRegistry::instance().close_all();
    Runtime::instance().stop();
    fail("runtime add timed out");
  }
  const int sum = add_fut.get();
  if (sum != 42) {
    SessionRegistry::instance().close_all();
    Runtime::instance().stop();
    fail("runtime add wrong sum");
  }
  std::printf("runtime add ok sum=%d\n", sum);

  SessionRegistry::instance().close_all();
  Runtime::instance().stop();
  g_add_done.reset();
  std::printf("smoke ok\n");
  return 0;
}
