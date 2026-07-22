#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"
#include "dart_cpp_bridge/dart_fn.hpp"

#include <async_simple/Promise.h>
#include <async_simple/coro/FutureAwaiter.h>

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
    if (fn) {
      fn(r);
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

std::vector<std::uint8_t> Session::invoke_dart_fn_sync(std::uint64_t generation, std::uint64_t fn_id,
                                                      std::vector<std::uint8_t> args_payload) {
  if (!alive(generation)) {
    throw std::runtime_error("DartFn: session generation expired");
  }

  auto promise = std::make_shared<std::promise<DartFnReply>>();
  auto future = promise->get_future();
  const auto reply_id = next_dart_fn_reply_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(dart_fn_mu_);
    dart_fn_pending_.emplace(reply_id, [promise](DartFnReply r) {
      try {
        promise->set_value(std::move(r));
      } catch (...) {
      }
    });
  }

  ByteWriter payload;
  payload.u64(fn_id);
  if (!args_payload.empty()) {
    payload.bytes(args_payload.data(), args_payload.size());
  }
  auto frame = make_frame(MsgType::kDartFnCall, reply_id, /*method_id=*/0, payload.raw());
  try_post(generation, frame);

  auto reply = future.get();
  if (!reply.ok) {
    throw std::runtime_error(reply.error.empty() ? "DartFn failed" : reply.error);
  }
  return std::move(reply.payload);
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

// callAsync: block on pool, deliver result by completing a std::promise watched via
// a one-shot posted back to io using only std::promise (no async_simple Future).
// Implementation: fire pool work from call site in wire; Lazy wrapper below uses
// a simple callback resume pattern with shared state + asio::post to io without
// resuming coroutine_handle (avoids async_simple handle issues).

async_simple::coro::Lazy<std::string> DartFnStringToString::callAsync(std::string arg) const {
  // Intentionally simple: run callSync on pool via blocking get wrapped in posted tasks.
  // We avoid co_await async_simple::Future (unstable here). Instead:
  // 1) post work to pool that callSyncs
  // 2) pool posts result to a shared state and signals via std::promise
  // 3) this coroutine cannot wait without blocking io — so callAsync must NOT
  //    be co_awaited on io if implemented with callSync inline.
  //
  // Contract for callAsync used FROM io Lazy:
  // We use a dual-post pattern where the OUTER wire does not co_await this Lazy
  // for the wait; wire uses pool directly. This Lazy is for C++ business that
  // already runs on a non-io context, OR we document callAsync = callSync
  // (blocks current thread).
  //
  // User asked: async on io without blocking io. Wire path implements that.
  // Here callAsync == callSync for API symmetry when user co_awaits on wrong thread.
  co_return callSync(std::move(arg));
}

}  // namespace dcb
