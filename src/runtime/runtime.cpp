#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

namespace dcb {

Runtime& Runtime::instance() {
  static Runtime rt;
  return rt;
}

Runtime::Runtime() = default;
Runtime::~Runtime() { stop(); }

void Runtime::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  io_.restart();
  pool_ = std::make_unique<asio::thread_pool>(4);
  guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
      asio::make_work_guard(io_));
  io_thread_ = std::make_unique<std::thread>([this] { io_.run(); });
}

void Runtime::stop() {
  if (!started_.exchange(false)) {
    return;
  }
  if (guard_) {
    guard_->reset();
    guard_.reset();
  }
  io_.stop();
  if (io_thread_ && io_thread_->joinable()) {
    io_thread_->join();
  }
  io_thread_.reset();
  if (pool_) {
    pool_->join();
    pool_.reset();
  }
}

void Session::set_stream_open(std::uint64_t stream_id, bool open) {
  std::lock_guard lock(streams_mu_);
  if (open) {
    streams_open_[stream_id] = true;
  } else {
    streams_open_.erase(stream_id);
  }
}

bool Session::stream_open(std::uint64_t stream_id) const {
  std::lock_guard lock(streams_mu_);
  auto it = streams_open_.find(stream_id);
  return it != streams_open_.end() && it->second;
}

SessionRegistry& SessionRegistry::instance() {
  static SessionRegistry reg;
  return reg;
}

std::uint64_t SessionRegistry::open(std::int64_t reply_port) {
  const auto id = next_id_.fetch_add(1, std::memory_order_relaxed);
  auto session = std::make_shared<Session>(reply_port);
  std::lock_guard lock(mu_);
  sessions_.emplace(id, std::move(session));
  return id;
}

std::shared_ptr<Session> SessionRegistry::get(std::uint64_t id) const {
  std::lock_guard lock(mu_);
  auto it = sessions_.find(id);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return it->second;
}

void SessionRegistry::close(std::uint64_t id) {
  std::shared_ptr<Session> s;
  {
    std::lock_guard lock(mu_);
    auto it = sessions_.find(id);
    if (it == sessions_.end()) {
      return;
    }
    s = std::move(it->second);
    sessions_.erase(it);
  }
  if (s) {
    s->dispose();
  }
}

void SessionRegistry::close_all() {
  std::unordered_map<std::uint64_t, std::shared_ptr<Session>> tmp;
  {
    std::lock_guard lock(mu_);
    tmp.swap(sessions_);
  }
  for (auto& kv : tmp) {
    if (kv.second) {
      kv.second->dispose();
    }
  }
}

}  // namespace dcb
