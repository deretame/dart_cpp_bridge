#pragma once

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/runtime.hpp"

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dcb {

struct DartFnReply {
  bool ok{false};
  std::vector<std::uint8_t> payload;
  std::string error;
};

class Session {
 public:
  explicit Session(std::int64_t reply_port) : reply_port_(reply_port) {}

  std::int64_t reply_port() const { return reply_port_; }
  std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }

  void dispose();

  bool alive(std::uint64_t gen) const {
    return !disposed_.load(std::memory_order_acquire) &&
           generation_.load(std::memory_order_acquire) == gen;
  }

  // Run a short registration/dispatch critical section only while this
  // Session is still alive. Session::dispose() takes the same mutex before
  // invalidating the generation, so callers can atomically register a late
  // result or reject it after disposal.
  template <typename Fn>
  bool with_alive(Fn&& fn) {
    std::lock_guard lock(dart_fn_mu_);
    if (disposed_.load(std::memory_order_relaxed)) {
      return false;
    }
    std::forward<Fn>(fn)();
    return true;
  }

  void try_post(std::uint64_t gen, const std::vector<std::uint8_t>& frame) {
    if (!alive(gen)) {
      return;
    }
    Runtime::instance().post_to_dart(reply_port_, frame.data(), frame.size());
  }

  void set_stream_open(std::uint64_t stream_id, bool open);
  bool stream_open(std::uint64_t stream_id) const;

  // True async: returns the receiver side of an internal oneshot channel that
  // resolves to the raw Dart reply payload. Suspends without blocking the io
  // thread. May throw synchronously if the session generation is expired
  // (call on a thread where a throw is caught, i.e. inside a sender chain's
  // setup on the io thread). The caller is responsible for migrating the
  // completion to the io thread (e.g. via detail::dartfn_sender).
  co::oneshot::Receiver<DartFnReply> invoke_dart_fn_async(
      std::uint64_t generation, std::uint64_t fn_id, std::vector<std::uint8_t> args_payload);

  void complete_dart_fn(std::uint64_t reply_id, bool ok, std::vector<std::uint8_t> payload,
                        std::string error);

 private:
  using CompleteFn = std::function<void(DartFnReply)>;

  std::int64_t reply_port_{0};
  std::atomic<std::uint64_t> generation_{1};
  std::atomic<bool> disposed_{false};
  mutable std::mutex streams_mu_;
  std::unordered_map<std::uint64_t, bool> streams_open_;

  std::mutex dart_fn_mu_;
  std::atomic<std::uint64_t> next_dart_fn_reply_{1};
  std::unordered_map<std::uint64_t, CompleteFn> dart_fn_pending_;
};

class SessionRegistry {
 public:
  static SessionRegistry& instance();

  std::uint64_t open(std::int64_t reply_port);
  std::shared_ptr<Session> get(std::uint64_t id) const;
  /// Look up a session's ID from its shared_ptr (used for pure C API bridging). Returns 0 if not found.
  std::uint64_t find_id(const std::shared_ptr<Session>& s) const;
  void close(std::uint64_t id);
  void close_all();

 private:
  SessionRegistry() = default;

  mutable std::mutex mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  std::atomic<std::uint64_t> next_id_{1};
};

}  // namespace dcb
