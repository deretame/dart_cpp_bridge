#pragma once
// Parse-only stub for codegen (real build uses FetchContent async-simple).
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace async_simple {

// Signal types used by the runtime headers (asio_executor.hpp /
// foreign_executor.hpp) for cancellable sleep.
enum SignalType : uint64_t {
  None = 0,
  Terminate = 1,  // trigger-once signal used for cancellation
  All = UINT64_MAX,
};

class Slot;  // defined in Executor.h stub

// Minimal Signal stub: only the surface referenced by the runtime headers
// (emits + create) is declared.
class Signal {
 public:
  SignalType emits(SignalType state) noexcept { return state; }

  template <typename T = Signal>
  static std::shared_ptr<T> create() {
    return std::make_shared<T>();
  }

  virtual ~Signal() = default;
};

// Thrown by awaiters when a Terminate signal interrupts them.
class SignalException : public std::runtime_error {
 public:
  explicit SignalException(SignalType signal, std::string msg = "")
      : std::runtime_error(std::move(msg)), signal_(signal) {}
  SignalType value() const { return signal_; }

 private:
  SignalType signal_;
};

// Helper used by executor timer paths to register/check Terminate handlers.
struct signalHelper {
  signalHelper(SignalType sign) : sign_(sign) {}

  template <typename... Args>
  bool tryEmplace(Slot* slot, Args&&...) noexcept {
    return slot != nullptr;
  }

  void checkHasCanceled(Slot* slot, std::string error_msg = "") {
    (void)slot;
    (void)error_msg;
  }

  bool hasCanceled(const Slot* slot) noexcept { return slot != nullptr; }

  SignalType sign_;
};

}  // namespace async_simple
