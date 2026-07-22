#pragma once

#include <async_simple/Executor.h>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/dispatch.hpp>
#include <asio/steady_timer.hpp>

#include <chrono>
#include <memory>
#include <utility>

namespace dcb {

// Schedule async_simple tasks onto asio::io_context (single-threaded OK).
class AsioExecutor : public async_simple::Executor {
 public:
  explicit AsioExecutor(asio::io_context& ioc) : ioc_(ioc) {}

  bool schedule(Func func) override {
    asio::post(ioc_, std::move(func));
    return true;
  }

  bool schedule(Func func, uint64_t schedule_info) override {
    if ((schedule_info & 0xF) >=
        static_cast<uint64_t>(async_simple::Executor::Priority::YIELD)) {
      asio::post(ioc_, std::move(func));
    } else {
      asio::dispatch(ioc_, std::move(func));
    }
    return true;
  }

  bool checkin(Func func, void* /*ctx*/) override {
    asio::dispatch(ioc_, std::move(func));
    return true;
  }

  void* checkout() override { return &ioc_; }

  bool currentThreadInExecutor() const override { return true; }

  size_t currentContextId() const override {
    return reinterpret_cast<size_t>(&ioc_);
  }

 private:
  void schedule(Func func, Duration dur) override {
    auto timer = std::make_shared<asio::steady_timer>(ioc_, dur);
    timer->async_wait([fn = std::move(func), timer](const asio::error_code&) { fn(); });
  }

  asio::io_context& ioc_;
};

}  // namespace dcb
