#pragma once
// Parse-only stub for codegen (real build uses FetchContent async-simple).
#include "async_simple/Future.h"

#include <exception>
#include <utility>

namespace async_simple {

// Minimal Promise stub: produces a Future.
template <typename T>
class Promise {
 public:
  Promise() = default;
  Promise(Promise&&) = default;
  Promise& operator=(Promise&&) = default;

  Future<T> getFuture() { return Future<T>{}; }
  void setValue(T val) {}
  void setException(std::exception_ptr) {}
};

}  // namespace async_simple
