#pragma once
// Parse-only stub for codegen (real build uses FetchContent async-simple).
#include "async_simple/Future.h"

namespace async_simple {
namespace coro {

// FutureAwaiter allows co_await on a Future.
template <typename T>
class FutureAwaiter {
 public:
  explicit FutureAwaiter(Future<T> fut) : fut_(std::move(fut)) {}

  bool await_ready() const { return true; }
  void await_suspend(std::coroutine_handle<>) {}
  T await_resume() { return fut_.value(); }

 private:
  Future<T> fut_;
};

}  // namespace coro
}  // namespace async_simple
