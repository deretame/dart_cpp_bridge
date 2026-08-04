#pragma once

// P2300 (std::execution / stdexec) scheduler adapter that runs sender work on
// the runtime's asio::io_context event loop — the single-threaded io thread.
// Design reference: docs/cpp26_executor_model_usage.md §12.6 (Asio 适配).
//
// `stdexec::schedule(*sched)` returns the official exec::asio adapter sender
// for `asio::post(io_context, use_sender)`: the posted handler runs on the io
// thread, so every step of a chain launched with
// `stdexec::starts_on(*sched, ...)` executes on the io thread, and its
// completion fires back on the io thread. Error/stopped mapping follows the
// adapter contract: operation_aborted / operation_canceled -> set_stopped,
// other error_code -> set_error(std::exception_ptr).
//
// Timers (dcb::sleep, declared in runtime.hpp) use the same adapter via
// asio::steady_timer + async_wait(use_sender), so sleeping never blocks the
// io thread and a stop request cancels the timer.
//
// Lifetime: pending schedule operations capture the io_context reference, so
// the scheduler must not outlive the io_context. The Runtime owns both and
// guarantees the order: io_context member declared before the scheduler
// member; stop() joins the io thread before destruction.
//
// The scheduler is trivially copyable-equivalent in the sense required by the
// stdexec scheduler concept: copies share the underlying io_context pointer
// plus a small shared state (thread-id bookkeeping for the deadlock guard).

#include <stdexec/execution.hpp>

#include <exec/asio/asio_config.hpp>
#include <exec/asio/use_sender.hpp>

#include <asio/io_context.hpp>

#include <atomic>
#include <memory>
#include <thread>

namespace dcb {

class IoContextScheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;

  explicit IoContextScheduler(asio::io_context& ioc)
      : ioc_(&ioc), state_(std::make_shared<State>()) {}

  // Adapter sender: posts a no-op handler onto the io_context; completes on
  // the io thread with set_value().
  stdexec::sender auto schedule() const noexcept {
    return exec::asio::asio_impl::post(*ioc_, exec::asio::use_sender);
  }

  asio::io_context& io() const noexcept { return *ioc_; }

  asio::io_context::executor_type executor() const noexcept {
    return ioc_->get_executor();
  }

  // True only when the calling thread is the thread that runs the io_context.
  // Used by dcb::sync_wait (runtime.hpp) as a deadlock guard: it refuses to
  // block a thread that the awaited sender depends on.
  bool current_thread_is_io() const noexcept {
    return std::this_thread::get_id() ==
           state_->io_thread_id.load(std::memory_order_acquire);
  }

  // Called by the owner (Runtime) right after it spawns the io thread.
  void set_io_thread_id(std::thread::id id) {
    state_->io_thread_id.store(id, std::memory_order_release);
  }

  // Two schedulers are equal when they wrap the same io_context.
  friend bool operator==(const IoContextScheduler&, const IoContextScheduler&) = default;

 private:
  // Shared mutable state keeps the scheduler copyable: std::atomic is not
  // copyable, and the scheduler concept requires copy constructibility.
  struct State {
    std::atomic<std::thread::id> io_thread_id{};
  };

  asio::io_context* ioc_;
  std::shared_ptr<State> state_;
};

}  // namespace dcb
