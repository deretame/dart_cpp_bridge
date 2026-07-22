#pragma once

#include "dart_cpp_bridge/runtime.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dcb {

// One session per Dart isolate (own reply port). Runtime is process-wide.
class Session {
 public:
  explicit Session(std::int64_t reply_port) : reply_port_(reply_port) {}

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

class SessionRegistry {
 public:
  static SessionRegistry& instance();

  std::uint64_t open(std::int64_t reply_port);
  std::shared_ptr<Session> get(std::uint64_t id) const;
  void close(std::uint64_t id);
  void close_all();

 private:
  SessionRegistry() = default;

  mutable std::mutex mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
  std::atomic<std::uint64_t> next_id_{1};
};

}  // namespace dcb
