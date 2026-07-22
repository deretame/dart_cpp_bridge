#pragma once

#include "dart_cpp_bridge/asio_executor.hpp"

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <async_simple/coro/Lazy.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
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
  DartPostFn post_fn_{nullptr};
  void* post_userdata_{nullptr};
};

}  // namespace dcb
