#pragma once

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dcb {

// FRB-style Dart callback (String -> String).
//
// callSync:  block current thread until Dart replies (no babysitting / offload).
// callAsync: schedule blocking wait on thread_pool, then resume Lazy on io
//            (io itself is not blocked; pool thread is occupied for the wait).
//
// Stalling io with callSync is the caller's choice/problem.
class DartFnStringToString {
 public:
  DartFnStringToString() = default;
  DartFnStringToString(std::shared_ptr<Session> session, std::uint64_t generation,
                       std::uint64_t fn_id)
      : session_(std::move(session)), generation_(generation), fn_id_(fn_id) {}

  explicit operator bool() const { return static_cast<bool>(session_) && fn_id_ != 0; }

  std::string callSync(std::string arg) const {
    if (!session_) {
      throw std::runtime_error("DartFn: empty");
    }
    ByteWriter args;
    args.str(arg);
    auto raw = session_->invoke_dart_fn_sync(generation_, fn_id_, args.raw());
    ByteReader r(raw.data(), raw.size());
    return r.str();
  }

  // Non-blocking for io_context: wait happens on thread_pool, result returns via Lazy.
  async_simple::coro::Lazy<std::string> callAsync(std::string arg) const;

 private:
  std::shared_ptr<Session> session_;
  std::uint64_t generation_{0};
  std::uint64_t fn_id_{0};
};

}  // namespace dcb
