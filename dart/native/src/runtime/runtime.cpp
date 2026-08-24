#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"
#include "dart_cpp_bridge/channel.hpp"
#include "dart_cpp_bridge/object_handle.hpp"

#include <iostream>
#include <utility>

namespace dcb {

// Implemented by cbridge.cpp. Runtime stop must cancel DartFn calls that were
// accepted but whose io scheduler operation has not started yet.
void cancel_pending_dart_fn_calls() noexcept;

Runtime& Runtime::instance() {
  static Runtime rt;
  return rt;
}

Runtime::Runtime() = default;
Runtime::~Runtime() { stop(); }

void Runtime::start() {
  std::lock_guard<std::mutex> lock(start_stop_mu_);
  if (lifecycle_ == Lifecycle::kRunning) {
    return;
  }
  if (lifecycle_ == Lifecycle::kStopping) {
    throw std::logic_error("runtime is stopping");
  }
  lifecycle_ = Lifecycle::kStarting;
  io_.restart();
  try {
    pool_ = std::make_unique<exec::asio::asio_thread_pool>(pool_threads_);
    guard_ = std::make_unique<DCB_ASIO_NS::executor_work_guard<DCB_ASIO_NS::io_context::executor_type>>(
        DCB_ASIO_NS::make_work_guard(io_));
    io_threads_.reserve(io_threads_count_);
    for (std::uint32_t i = 0; i < io_threads_count_; ++i) {
      io_threads_.emplace_back([this] { io_.run(); });
    }
    // Publish running only after every required resource exists. A failed
    // start therefore cannot leave later callers in a fake running state.
    started_.store(true, std::memory_order_release);
    lifecycle_ = Lifecycle::kRunning;
  } catch (...) {
    if (guard_) {
      guard_->reset();
      guard_.reset();
    }
    io_.stop();
    for (auto& thread : io_threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    io_threads_.clear();
    pool_.reset();
    started_.store(false, std::memory_order_release);
    lifecycle_ = Lifecycle::kStopped;
    throw;
  }
  // current_thread_is_io() (used by sync_wait's deadlock guard) queries
  // asio's running_in_this_thread() and needs no bookkeeping here.
}

void Runtime::stop() {
  std::unique_lock<std::mutex> lock(start_stop_mu_);
  if (lifecycle_ == Lifecycle::kStopped || lifecycle_ == Lifecycle::kStopping) {
    return;
  }
  if (io_.get_executor().running_in_this_thread()) {
    // Reject self-stop before changing lifecycle state or taking ownership of
    // resources. The public shutdown contract requires the main isolate.
    throw std::logic_error("Runtime::stop() cannot run on the io thread");
  }
  lifecycle_ = Lifecycle::kStopping;
  started_.store(false, std::memory_order_release);

  // Take ownership of runtime resources while protected by the gate.
  // Everything below can run user code or block and must not hold the gate:
  // shutdown callbacks may re-enter the public API.
  auto guard = std::move(guard_);
  auto io_threads = std::move(io_threads_);
  auto pool = std::move(pool_);
  lock.unlock();

  cancel_pending_dart_fn_calls();
  if (guard) {
    guard->reset();
  }
  io_.stop();
  for (auto& thread : io_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  // asio_thread_pool's destructor stops and joins the pool threads.
  pool.reset();

  lock.lock();
  lifecycle_ = Lifecycle::kStopped;
}

void Session::dispose() {
  std::vector<CompleteFn> abandoned;
  {
    std::lock_guard lock(dart_fn_mu_);
    if (disposed_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    // Invalidate the generation and detach pending callbacks in the same
    // critical section as DartFn registration. Otherwise a caller can pass
    // the alive() check, be pre-empted by dispose(), and insert a callback
    // after the pending map has been cleared.
    generation_.fetch_add(1, std::memory_order_acq_rel);
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
    if (!alive(generation)) {
      throw std::runtime_error("DartFn: session generation expired");
    }
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
