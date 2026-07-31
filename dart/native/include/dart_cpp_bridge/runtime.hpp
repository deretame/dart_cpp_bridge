#pragma once

#include "dart_cpp_bridge/asio_executor.hpp"

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <async_simple/Future.h>
#include <async_simple/Promise.h>
#include <async_simple/Unit.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace dcb {

using DartPostFn = void (*)(std::int64_t port, const std::uint8_t* data, std::size_t len,
                            void* userdata);

class Runtime {
 public:
  static Runtime& instance();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  void start();
  void stop();
  bool running() const { return started_.load(std::memory_order_acquire); }

  /// Set thread pool size (must be called before start; default 4).
  void set_pool_threads(std::uint32_t n) { pool_threads_ = n ? n : 1; }

  void ensure_running() {
    if (!running()) {
      throw std::runtime_error("runtime stopped");
    }
  }

  asio::io_context& io() { return io_; }
  asio::thread_pool& pool() { return *pool_; }
  AsioExecutor* executor() { return executor_.get(); }

  // Post Lazy onto io, then via(AsioExecutor).start so oneshot can schedule resume.
  //
  // IMPORTANT: keep `factory` alive until the Lazy completes. Coroutine lambdas may
  // reference captures from the lambda object; destroying it after start() races.
  template <class LazyFactory>
  void spawn_on_asio(LazyFactory&& factory) {
    ensure_running();
    auto* ex = executor_.get();
    if (!ex) {
      throw std::runtime_error("runtime executor missing");
    }
    asio::post(io_, [factory = std::forward<LazyFactory>(factory), ex]() mutable {
      auto holder = std::make_shared<std::decay_t<decltype(factory)>>(std::move(factory));
      auto lazy = (*holder)();
      std::move(lazy).via(ex).start([holder](auto&&) {
        (void)holder;
      });
    });
  }

  void set_dart_post(DartPostFn fn, void* userdata) {
    post_fn_ = fn;
    post_userdata_ = userdata;
  }

  void post_to_dart(std::int64_t port, const std::uint8_t* data, std::size_t len) {
    if (!running()) {
      return;
    }
    auto fn = post_fn_;
    if (fn) {
      fn(port, data, len, post_userdata_);
    }
  }

 private:
  Runtime();
  ~Runtime();

  asio::io_context io_;
  std::unique_ptr<AsioExecutor> executor_;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
  std::unique_ptr<std::thread> io_thread_;
  std::unique_ptr<asio::thread_pool> pool_;
  std::atomic<bool> started_{false};
  std::uint32_t pool_threads_{4};
  DartPostFn post_fn_{nullptr};
  void* post_userdata_{nullptr};
  std::mutex start_stop_mu_;
};

// Launch a Lazy on the runtime's io executor from a NON-coroutine context.
//
// Returns a RescheduleLazy already bound to the io executor but NOT yet
// started (it is lazy) — the caller chooses how to trigger it:
//
//   // 1) Block a normal (non-io) thread until the result is ready: the value
//   //    is returned, or the coroutine's exception is rethrown. Preferred
//   //    blocking API — no std::promise/future plumbing needed
//   //    (include <async_simple/coro/SyncAwait.h>):
//   int v = async_simple::coro::syncAwait(dcb::spawn(std::move(lazy)));
//
//   // 2) Fire-and-forget (discard both value and exception):
//   dcb::spawn_detached(std::move(lazy));
//   //    equivalent to: dcb::spawn(std::move(lazy)).start([](auto&&) {});
//
//   // 3) Custom completion callback:
//   dcb::spawn(std::move(lazy)).start([](async_simple::Try<int>&& t) { ... });
//
// If you are already INSIDE a coroutine running on io, do not use spawn — just
// `co_await std::move(lazy)` directly: it inherits the current executor and
// rethrows exceptions. co_await on the returned RescheduleLazy is deprecated /
// forbidden by async_simple.
//
// SAFETY:
//   - syncAwait must NOT be called on the io thread: it blocks its caller, and
//     blocking the io thread while the awaited Lazy needs it is a self
//     deadlock. AsioExecutor::currentThreadInExecutor() identifies the io
//     thread and syncAwait asserts on it.
//   - Do NOT use async_simple's RescheduleLazy::detach(): its completion
//     callback rethrows the coroutine's exception on the io thread, which
//     escapes into the event loop and terminates the io thread. Use
//     dcb::spawn_detached (empty callback) instead.
template <class T>
async_simple::coro::RescheduleLazy<T> spawn(async_simple::coro::Lazy<T> lazy) {
  auto& rt = Runtime::instance();
  rt.ensure_running();
  auto* ex = rt.executor();
  if (!ex) {
    throw std::runtime_error("runtime executor missing");
  }
  return std::move(lazy).via(ex);
}

// Fire-and-forget: launch a Lazy on the io executor and discard its result.
// The coroutine is started immediately on the io thread; both its value and
// any exception it throws are swallowed (that is the "detach" semantic).
//
//   dcb::spawn_detached(std::move(lazy));
//
// This is a safe wrapper over `spawn(lazy).start(empty_callback)`. Do NOT use
// async_simple's built-in RescheduleLazy::detach() instead: its callback
// rethrows the coroutine's exception on the io thread, which escapes into the
// event loop and terminates the io thread.
template <class T>
void spawn_detached(async_simple::coro::Lazy<T> lazy) {
  spawn(std::move(lazy)).start([](auto&&) {});
}

// Run a blocking callable on the runtime's thread pool and return a Lazy that
// resolves to its result. The io thread is never blocked: the callable runs on
// a pool thread while the awaiting coroutine suspends.
//
//   // inside a coroutine running on io:
//   auto v = co_await dcb::spawn_blocking([&] { return heavyComputation(); });
//
//   // block a normal (non-io) thread for the result:
//   auto v = async_simple::coro::syncAwait(dcb::spawn(dcb::spawn_blocking(f)));
//
//   // fire-and-forget from a non-coroutine context:
//   dcb::spawn_detached(dcb::spawn_blocking(f));
//
// Exceptions thrown by the callable are captured on the pool thread via
// Promise::setException and rethrown at the awaiter by the FutureAwaiter.
// Supports both value-returning and void callables.
//
// Implementation note: void callables are bridged through async_simple::Unit
// (Promise<Unit>/Future<Unit>) rather than Promise<void>, because
// FutureAwaiter<void>::await_resume does not call Future::value() and would
// silently drop an exception set via setException. Awaiting a Future<Unit>
// always goes through value(), so the exception is rethrown uniformly.
template <class F>
auto spawn_blocking(F&& f)
    -> async_simple::coro::Lazy<std::invoke_result_t<std::decay_t<F>>> {
  using T = std::invoke_result_t<std::decay_t<F>>;
  // void is bridged as Unit so that co_await rethrows exceptions (see note).
  using WireT = std::conditional_t<std::is_void_v<T>, async_simple::Unit, T>;
  auto& rt = Runtime::instance();
  rt.ensure_running();
  async_simple::Promise<WireT> p;
  auto fut = p.getFuture();
  asio::post(rt.pool(), [f = std::forward<F>(f), p = std::move(p)]() mutable {
    try {
      if constexpr (std::is_void_v<T>) {
        f();
        p.setValue(async_simple::Unit{});
      } else {
        p.setValue(f());
      }
    } catch (...) {
      p.setException(std::current_exception());
    }
  });
  if constexpr (std::is_void_v<T>) {
    co_await std::move(fut);
    co_return;
  } else {
    co_return co_await std::move(fut);
  }
}

}  // namespace dcb
