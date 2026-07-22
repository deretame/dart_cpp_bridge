#pragma once

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>
#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <cstdint>
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

  void ensure_running() {
    if (!running()) {
      throw std::runtime_error("runtime stopped");
    }
  }

  asio::io_context& io() { return io_; }
  asio::thread_pool& pool() { return *pool_; }

  template <class LazyFactory>
  void spawn_on_asio(LazyFactory&& factory) {
    ensure_running();
    asio::post(io_, [factory = std::forward<LazyFactory>(factory)]() mutable {
      auto lazy = factory();
      std::move(lazy).start([](auto&&) {});
    });
  }

  template <class F>
  auto spawn_blocking(F func)
      -> async_simple::coro::Lazy<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
    ensure_running();
    auto promise = std::make_shared<async_simple::Promise<R>>();
    auto future = promise->getFuture();
    auto* io = &io_;
    asio::post(*pool_, [promise, func = std::move(func), io]() mutable {
      try {
        if constexpr (std::is_void_v<R>) {
          func();
          asio::post(*io, [promise]() { promise->setValue(); });
        } else {
          R value = func();
          asio::post(*io, [promise, value = std::move(value)]() mutable {
            promise->setValue(std::move(value));
          });
        }
      } catch (...) {
        auto ep = std::current_exception();
        asio::post(*io, [promise, ep]() { promise->setException(ep); });
      }
    });
    if constexpr (std::is_void_v<R>) {
      co_await std::move(future);
      co_return;
    } else {
      co_return co_await std::move(future);
    }
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
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> guard_;
  std::unique_ptr<std::thread> io_thread_;
  std::unique_ptr<asio::thread_pool> pool_;
  std::atomic<bool> started_{false};
  DartPostFn post_fn_{nullptr};
  void* post_userdata_{nullptr};
};

}  // namespace dcb
