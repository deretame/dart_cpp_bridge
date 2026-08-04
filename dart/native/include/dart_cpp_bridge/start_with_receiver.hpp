#pragma once

// start_with_receiver -- stdexec connect+start helper with a user-controlled
// receiver.
//
// `dcb::start_with_receiver(sndr, rcvr)` eagerly connects `sndr` to `rcvr`,
// heap-allocates the resulting operation state, and starts it. The operation
// state deletes itself when a completion signal fires. This mirrors the
// lifetime pattern used by `exec::start_detached`, but forwards *all* three
// completion channels (`set_value`, `set_error`, `set_stopped`) to the caller's
// receiver and propagates the receiver's environment.
//
// Use this when you need a custom receiver (e.g. to turn completion into a Dart
// response frame) and the built-in `exec::start_detached` receiver is
// unsuitable because it terminates on `set_error`.
//
// Reference: docs/cpp26_executor_model_usage.md §5.4 / §13.2.

#include <stdexec/execution.hpp>

#include <exception>
#include <type_traits>
#include <utility>

namespace dcb {

namespace detail {

// Base type that stores the user receiver. The virtual destructor lets the
// internal receiver delete the operation state through a base pointer without
// knowing the concrete sender type.
template <typename Rcvr>
struct __start_with_receiver_op_base {
  virtual ~__start_with_receiver_op_base() = default;

  Rcvr rcvr_;

  explicit __start_with_receiver_op_base(Rcvr rcvr) : rcvr_(std::move(rcvr)) {}
};

// Internal receiver that forwards completion signals to the user receiver and
// deletes the heap-allocated operation state.
template <typename Rcvr>
struct __start_with_receiver_receiver {
  using receiver_concept = stdexec::receiver_tag;

  __start_with_receiver_op_base<Rcvr>* op_;

  template <class... As>
  void set_value(As&&... as) && noexcept {
    std::move(op_->rcvr_).set_value(std::forward<As>(as)...);
    delete op_;
  }

  void set_error(std::exception_ptr ep) && noexcept {
    std::move(op_->rcvr_).set_error(ep);
    delete op_;
  }

  void set_stopped() && noexcept {
    std::move(op_->rcvr_).set_stopped();
    delete op_;
  }

  auto get_env() const noexcept {
    if constexpr (requires { op_->rcvr_.get_env(); }) {
      return op_->rcvr_.get_env();
    } else {
      return stdexec::env<>{};
    }
  }
};

// Operation state: stores the user receiver (via the base) plus the connected
// stdexec operation state. Immovable; always heap-allocated.
template <stdexec::sender S, typename Rcvr>
struct __start_with_receiver_op : __start_with_receiver_op_base<Rcvr> {
  using __receiver_t = __start_with_receiver_receiver<Rcvr>;
  using op_t =
      decltype(stdexec::connect(std::declval<S>(), std::declval<__receiver_t>()));

  op_t op_;

  __start_with_receiver_op(S sndr, Rcvr rcvr)
      : __start_with_receiver_op_base<Rcvr>(std::move(rcvr)),
        op_(stdexec::connect(std::move(sndr), __receiver_t{this})) {}

  __start_with_receiver_op(const __start_with_receiver_op&) = delete;
  __start_with_receiver_op(__start_with_receiver_op&&) = delete;
  __start_with_receiver_op& operator=(const __start_with_receiver_op&) = delete;
  __start_with_receiver_op& operator=(__start_with_receiver_op&&) = delete;
};

}  // namespace detail

// Eagerly start `sndr` with the user-provided receiver `rcvr`.
//
// The operation state is heap-allocated and kept alive until the sender
// completes. Completion signals are forwarded to `rcvr`. `rcvr` must satisfy
// `stdexec::receiver` and accept the sender's completion signatures.
template <stdexec::sender S, typename Rcvr>
void start_with_receiver(S&& sndr, Rcvr rcvr) {
  using __op_t =
      detail::__start_with_receiver_op<std::decay_t<S>, std::decay_t<Rcvr>>;
  auto* state = new __op_t(std::forward<S>(sndr), std::move(rcvr));
  stdexec::start(state->op_);
}

}  // namespace dcb
