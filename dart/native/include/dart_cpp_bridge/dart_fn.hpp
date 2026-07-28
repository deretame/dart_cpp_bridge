#pragma once

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace dcb {

// FRB-style Dart callback with an arbitrary signature.
//
// Syntax mirrors std::function: DartFn<Ret(Args...)>.  Examples:
//   DartFn<std::string(std::string)>              // String -> String
//   DartFn<int(std::string)>                        // String -> int
//   DartFn<std::string(int, std::string)>           // (int, String) -> String
//   DartFn<void()>                                    // () -> void
//
// The encode/decode lambdas are supplied by generated wire code so that the
// runtime stays binary-agnostic.  Business code simply calls fn(args...).
//
// DartFn is a functor: operator() returns Lazy<Ret> (true async, non-blocking).
// The calling Lazy must be bound to an Executor (.via).
//
// For blocking contexts (thread pool, foreign threads), use syncAwait:
//   auto r = async_simple::coro::syncAwait(dcb::spawn(fn(args...)));
// Do NOT syncAwait on the io thread (self-deadlock).
template <typename>
class DartFn;

template <typename Ret, typename... Args>
class DartFn<Ret(Args...)> {
 public:
  using EncodeArgs = std::function<void(ByteWriter&, const Args&...)>;
  using DecodeRet = std::function<Ret(const std::uint8_t*, std::size_t)>;

  DartFn() = default;

  DartFn(std::shared_ptr<Session> session, std::uint64_t generation, std::uint64_t fn_id,
         EncodeArgs encode, DecodeRet decode)
      : session_(std::move(session)),
        generation_(generation),
        fn_id_(fn_id),
        encode_(std::move(encode)),
        decode_(std::move(decode)) {}

  // Convenience constructor for the legacy string -> string signature.
  // Only enabled when the signature is exactly std::string(std::string).
  DartFn(std::shared_ptr<Session> session, std::uint64_t generation, std::uint64_t fn_id)
      requires(std::is_same_v<std::tuple<Ret, Args...>, std::tuple<std::string, std::string>>)
      : DartFn(std::move(session), generation, fn_id,
               [](ByteWriter& w, const std::string& s) { w.str(s); },
               [](const std::uint8_t* d, std::size_t n) {
                 ByteReader r(d, n);
                 return r.str();
               }) {}

  explicit operator bool() const { return static_cast<bool>(session_) && fn_id_ != 0; }

  /// 获取关联的 session（用于 dcb_invoke_dart_fn 等纯 C API）。
  const std::shared_ptr<Session>& session() const { return session_; }
  /// 获取 Dart 闭包 ID（用于 dcb_invoke_dart_fn 等纯 C API）。
  std::uint64_t fn_id() const { return fn_id_; }

  // Functor interface: returns Lazy<Ret> (true async, non-blocking).
  // Suspends the calling coroutine until Dart replies via oneshot channel.
  // The calling Lazy must be bound to an Executor (.via).
  //
  // For blocking contexts, wrap with syncAwait:
  //   auto r = async_simple::coro::syncAwait(dcb::spawn(fn(args...)));
  async_simple::coro::Lazy<Ret> operator()(const Args&... args) const {
    if (!session_) {
      throw std::runtime_error("DartFn: empty");
    }
    ByteWriter w;
    encode_(w, args...);
    auto raw = co_await session_->invoke_dart_fn_async(generation_, fn_id_, w.raw());
    co_return decode_(raw.data(), raw.size());
  }

 private:
  std::shared_ptr<Session> session_;
  std::uint64_t generation_{0};
  std::uint64_t fn_id_{0};
  EncodeArgs encode_;
  DecodeRet decode_;
};

using DartFnStringToString = DartFn<std::string(std::string)>;

}  // namespace dcb
