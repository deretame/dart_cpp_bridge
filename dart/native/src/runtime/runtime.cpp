#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/object_handle.hpp"

#include <iostream>
#include <utility>

namespace dcb {

Runtime& Runtime::instance() {
  static Runtime rt;
  return rt;
}

Runtime::Runtime() = default;
Runtime::~Runtime() { stop(); }

void Runtime::start() {
  std::lock_guard<std::mutex> lock(start_stop_mu_);
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  io_.restart();
  pool_ = std::make_unique<exec::asio::asio_thread_pool>(pool_threads_);
  guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
      asio::make_work_guard(io_));
  io_thread_ = std::make_unique<std::thread>([this] { io_.run(); });
  // Tell the scheduler which thread runs the io_context so that
  // current_thread_is_io() (used by sync_wait's deadlock guard) is accurate.
  io_sched_.set_io_thread_id(io_thread_->get_id());
}

void Runtime::stop() {
  std::lock_guard<std::mutex> lock(start_stop_mu_);
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
    // asio_thread_pool's destructor stops and joins the pool threads.
    pool_.reset();
  }
}

void Session::dispose() {
  generation_.fetch_add(1, std::memory_order_acq_rel);
  std::vector<CompleteFn> abandoned;
  {
    std::lock_guard lock(dart_fn_mu_);
    for (auto& kv : dart_fn_pending_) {
      abandoned.push_back(std::move(kv.second));
    }
    dart_fn_pending_.clear();
  }
  DartFnReply r;
  r.ok = false;
  r.error = "session disposed";
  for (auto& fn : abandoned) {
    if (!fn) {
      continue;
    }
    try {
      fn(r);
    } catch (const std::exception& e) {
      std::cerr << "[dcb] Session::dispose() callback failed: " << e.what() << std::endl;
    } catch (...) {
      std::cerr << "[dcb] Session::dispose() callback failed: unknown exception" << std::endl;
    }
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

co::oneshot::Receiver<DartFnReply> Session::invoke_dart_fn_async(
    std::uint64_t generation, std::uint64_t fn_id, std::vector<std::uint8_t> args_payload) {
  if (!alive(generation)) {
    throw std::runtime_error("DartFn: session generation expired");
  }

  auto [tx, rx] = co::oneshot::channel<DartFnReply>();
  // shared_ptr: std::function requires copyable target; Sender is move-only.
  auto tx_holder = std::make_shared<co::oneshot::Sender<DartFnReply>>(std::move(tx));
  const auto reply_id = next_dart_fn_reply_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(dart_fn_mu_);
    dart_fn_pending_.emplace(reply_id, [tx_holder](DartFnReply r) {
      // Any thread (typically Dart FFI). The reply is moved back onto the io
      // thread by the continues_on in dartfn_sender (see dart_fn.hpp).
      (void)tx_holder->send(std::move(r));
    });
  }

  ByteWriter payload;
  payload.u64(fn_id);
  if (!args_payload.empty()) {
    payload.bytes(args_payload.data(), args_payload.size());
  }
  auto frame = make_frame(MsgType::kDartFnCall, reply_id, /*method_id=*/0, payload.raw());
  try_post(generation, frame);

  // Completion: set_value(std::optional<DartFnReply>) — std::nullopt if the
  // channel was closed (e.g. session disposed); set_error if send_error was
  // used. The reply completes on whichever thread called complete_dart_fn;
  // detail::dartfn_sender migrates it back to the io thread.
  return std::move(rx);
}

void Session::complete_dart_fn(std::uint64_t reply_id, bool ok, std::vector<std::uint8_t> payload,
                               std::string error) {
  CompleteFn fn;
  {
    std::lock_guard lock(dart_fn_mu_);
    auto it = dart_fn_pending_.find(reply_id);
    if (it == dart_fn_pending_.end()) {
      return;
    }
    fn = std::move(it->second);
    dart_fn_pending_.erase(it);
  }
  if (!fn) {
    return;
  }
  DartFnReply r;
  r.ok = ok;
  r.payload = std::move(payload);
  r.error = std::move(error);
  fn(std::move(r));
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

std::uint64_t SessionRegistry::find_id(const std::shared_ptr<Session>& s) const {
  std::lock_guard lock(mu_);
  for (const auto& kv : sessions_) {
    if (kv.second == s) {
      return kv.first;
    }
  }
  return 0;
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
  ObjectHandleRegistry::instance().drop_all(id);
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
    ObjectHandleRegistry::instance().drop_all(kv.first);
  }
}

}  // namespace dcb
