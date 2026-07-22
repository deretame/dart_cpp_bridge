#pragma once

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dcb {

// FRB-style Dart callback handle (String -> String).
// call() blocks the current thread until Dart replies — invoke only from thread_pool,
// never from the single io_context thread.
class DartFnStringToString {
 public:
  DartFnStringToString() = default;
  DartFnStringToString(std::shared_ptr<Session> session, std::uint64_t generation,
                       std::uint64_t fn_id)
      : session_(std::move(session)), generation_(generation), fn_id_(fn_id) {}

  explicit operator bool() const { return static_cast<bool>(session_) && fn_id_ != 0; }

  std::string call(std::string arg) const {
    if (!session_) {
      throw std::runtime_error("DartFn: empty");
    }
    ByteWriter args;
    args.str(arg);
    auto raw = session_->invoke_dart_fn_blocking(generation_, fn_id_, args.raw());
    ByteReader r(raw.data(), raw.size());
    return r.str();
  }

 private:
  std::shared_ptr<Session> session_;
  std::uint64_t generation_{0};
  std::uint64_t fn_id_{0};
};

}  // namespace dcb
