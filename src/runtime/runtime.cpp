#include "dart_cpp_bridge/runtime.hpp"

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

Session& global_session() {
  static Session s;
  return s;
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

}  // namespace dcb
