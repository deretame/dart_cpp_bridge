#pragma once

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <stdexec/execution.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace dcb {

namespace detail {

// Sender that performs a DartFn reverse call: posts to Dart, waits for the
// oneshot reply, migrates the completion to the io thread, decodes the
// payload, and completes with Ret. Errors (channel closed, Dart-side failure,
// decode failure) are delivered as set_error(std::exception_ptr).
template <typename Ret>
struct dartfn_sender {
  using sender_concept = stdexec::sender_tag;
  using completion_signatures = stdexec::completion_signatures<
    stdexec::set_value_t(Ret),
    stdexec::set_error_t(std::exception_ptr)>;

  using DecodeRet = std::function<Ret(const std::uint8_t*, std::size_t)>;

  // Decode step: unwraps the optional reply and decodes the payload.
  struct decode_fn {
    DecodeRet decode;

    Ret operator()(std::optional<DartFnReply> reply) const {
      if (!reply) {
        throw std::runtime_error("DartFn: channel closed");
      }
      if (!reply->ok) {
        throw std::runtime_error(reply->error.empty() ? "DartFn failed" : reply->error);
      }
      return decode(reply->payload.data(), reply->payload.size());
    }
  };

  using base_sender_t = decltype(
    std::declval<co::oneshot::Receiver<DartFnReply>>()
    | stdexec::continues_on(std::declval<const IoContextScheduler&>())
    | stdexec::then(std::declval<decode_fn>()));

  base_sender_t base_;

  dartfn_sender(co::oneshot::Receiver<DartFnReply> rx, const IoContextScheduler& sched,
                DecodeRet decode)
    : base_(std::move(rx)
            | stdexec::continues_on(sched)
            | stdexec::then(decode_fn{std::move(decode)})) {}

  dartfn_sender(dartfn_sender&&) = default;
  dartfn_sender& operator=(dartfn_sender&&) = default;
  dartfn_sender(const dartfn_sender&) = delete;
  dartfn_sender& operator=(const dartfn_sender&) = delete;

  auto get_env() const noexcept { return base_.get_env(); }

  template <stdexec::receiver Rcvr>
  auto connect(Rcvr rcvr) && {
    return stdexec::connect(std::move(base_), std::move(rcvr));
  }
};

}  // namespace detail

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
// DartFn is a functor: operator() returns a sender (true async, non-blocking,
// std::exec style). Its completion is delivered back on the io thread with
// the decoded return value; errors (Dart-side failure, channel closed) are
// delivered as set_error(std::exception_ptr).
//
// For blocking contexts (thread pool, foreign threads), use sync_wait:
//   auto r = dcb::sync_wait(fn(args...));
// Do NOT sync_wait on the io thread (self-deadlock).
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

  /// Get the associated session (for pure C APIs such as dcb_invoke_dart_fn).
  const std::shared_ptr<Session>& session() const { return session_; }
  /// Get the Dart closure ID (for pure C APIs such as dcb_invoke_dart_fn).
  std::uint64_t fn_id() const { return fn_id_; }

  // Functor interface: returns a sender (true async, non-blocking). The
  // calling coroutine can co_await it; the completion arrives on the io
  // thread. May throw synchronously (empty DartFn, encode failure) — call
  // from within a sender chain setup on the io thread.
  //
  // For blocking contexts, wrap with sync_wait:
  //   auto r = dcb::sync_wait(fn(args...));
  detail::dartfn_sender<Ret> operator()(const Args&... args) const {
    if (!session_) {
      throw std::runtime_error("DartFn: empty");
    }
    ByteWriter w;
    encode_(w, args...);
    auto* sched = Runtime::instance().io_scheduler();
    if (!sched) {
      throw std::runtime_error("runtime scheduler missing");
    }
    auto rx =
        session_->invoke_dart_fn_async(generation_, fn_id_, w.raw());
    return detail::dartfn_sender<Ret>{std::move(rx), *sched, decode_};
  }

 private:
  std::shared_ptr<Session> session_;
  std::uint64_t generation_{0};
  std::uint64_t fn_id_{0};
  EncodeArgs encode_;
  DecodeRet decode_;
};

}  // namespace dcb
