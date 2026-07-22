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
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dcb {

// Posts encoded frames back to the Dart isolate (Dart_PostCObject_DL).
using DartPostFn = void (*)(std::int64_t port, const std::uint8_t* data, std::size_t len,
                            void* userdata);

class Runtime {
 public:
  static Runtime& instance();

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  void start();
  void stop();

  asio::io_context& io() { return io_; }
  asio::thread_pool& pool() { return *pool_; }

  // Fire-and-forget: schedule a Lazy<> on the single io thread.
  template <class LazyFactory>
  void spawn_on_asio(LazyFactory&& factory) {
    asio::post(io_, [factory = std::forward<LazyFactory>(factory)]() mutable {
      auto lazy = factory();
      std::move(lazy).start([](auto&&) {});
    });
  }

  // Blocking work on thread_pool; completion posted back to io_context before resuming Lazy.
  template <class F>
  auto spawn_blocking(F func)
      -> async_simple::coro::Lazy<std::invoke_result_t<std::decay_t<F>>> {
    using R = std::invoke_result_t<std::decay_t<F>>;
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

class Session {
 public:
  Session() = default;

  void bind_reply_port(std::int64_t port) { reply_port_ = port; }
  std::int64_t reply_port() const { return reply_port_; }

  std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

  void dispose() { generation_.fetch_add(1, std::memory_order_acq_rel); }

  bool alive(std::uint64_t gen) const {
    return generation_.load(std::memory_order_acquire) == gen;
  }

  void try_post(std::uint64_t gen, const std::vector<std::uint8_t>& frame) {
    if (!alive(gen)) {
      return;
    }
    Runtime::instance().post_to_dart(reply_port_, frame.data(), frame.size());
  }

  void set_stream_open(std::uint64_t stream_id, bool open);
  bool stream_open(std::uint64_t stream_id) const;

 private:
  std::int64_t reply_port_{0};
  std::atomic<std::uint64_t> generation_{1};
  mutable std::mutex streams_mu_;
  std::unordered_map<std::uint64_t, bool> streams_open_;
};

Session& global_session();

}  // namespace dcb
