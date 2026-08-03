#pragma once

#include <stdexec/execution.hpp>

#include <exec/asio/asio_config.hpp>
#include <exec/asio/use_sender.hpp>

#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace dcb {

// Scheduler that runs std::exec work on the runtime's asio::io_context event
// loop (the single-threaded io thread).
//
// std::exec (stdexec) scheduler: `stdexec::schedule(*sched)` returns the
// official exec::asio adapter sender for `asio::post(io_context, use_sender)`
// — the posted handler runs on the io thread, so every step of a chain
// launched with stdexec::starts_on(*io_scheduler(), ...) executes on the io
// thread, and its completion fires back on the io thread.
//
// Timers (dcb::sleep) use the same adapter: asio::steady_timer +
// async_wait(use_sender), including the stop_token cancellation mapping
// (operation_aborted / operation_canceled -> set_stopped).
//
// Lifetime: pending schedule operations capture the io_context reference, so
// the scheduler must not outlive the io_context. Runtime guarantees this: the
// io_context member outlives the scheduler member, and stop() joins the io
// thread before the runtime is destroyed.
class IoContextScheduler {
 public:
  using scheduler_concept = stdexec::scheduler_tag;

  explicit IoContextScheduler(asio::io_context& ioc)
      : ioc_(&ioc), state_(std::make_shared<State>()) {}

  // Official adapter sender: completes on the io thread with set_value().
  stdexec::sender auto schedule() const noexcept {
    return exec::asio::asio_impl::post(*ioc_, exec::asio::use_sender);
  }

  asio::io_context& io() const noexcept { return *ioc_; }

  asio::io_context::executor_type executor() const noexcept {
    return ioc_->get_executor();
  }

  // True only when the calling thread is the thread that runs the io_context.
  // Used by dcb::sync_wait as a deadlock guard — it refuses to block a thread
  // that the awaited sender depends on.
  bool current_thread_is_io() const noexcept {
    return std::this_thread::get_id() ==
           state_->io_thread_id.load(std::memory_order_acquire);
  }

  // Called by the owner (Runtime) right after it spawns the io thread.
  void set_io_thread_id(std::thread::id id) {
    state_->io_thread_id.store(id, std::memory_order_release);
  }

  // Two schedulers are equal when they wrap the same io_context.
  bool operator==(const IoContextScheduler& o) const noexcept {
    return ioc_ == o.ioc_;
  }

 private:
  // Shared mutable state: keeps the scheduler copyable (std::atomic is
  // non-copyable, and the scheduler concept requires copy constructibility).
  struct State {
    std::atomic<std::thread::id> io_thread_id{};
  };

  asio::io_context* ioc_;
  std::shared_ptr<State> state_;
};

// Block the calling thread until `sndr` completes; returns the value or
// rethrows the sender's error. NEVER call on the io thread (self-deadlock).
// Implemented in runtime.hpp where the runtime singleton is available.
template <stdexec::sender S>
auto sync_wait(S&& sndr);

}  // namespace dcb
