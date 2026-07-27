#pragma once
// Parse-only stub for codegen (real build uses FetchContent async-simple).
#include <exception>
#include <utility>

namespace async_simple {

// Minimal Future stub: holds a value or exception.
template <typename T>
class Future {
 public:
  Future() = default;
  Future(Future&&) = default;
  Future& operator=(Future&&) = default;

  bool valid() const { return true; }
  T& value() { return val_; }
  const T& value() const { return val_; }

 private:
  T val_{};
};

}  // namespace async_simple
