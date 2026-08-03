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

// Shared control block holding the current opstate address; see runtime.hpp
// on_io_ctl for why inner receivers must not hold raw opstate pointers.
template <typename Op>
struct dartfn_ctl {
  Op* op{nullptr};
};

// Inner receiver of a dartfn_sender opstate; forwards the oneshot completion
// to the io thread before invoking the outer receiver.
template <typename Op>
struct dartfn_inner_receiver {
  using receiver_concept = stdexec::receiver_tag;

  std::shared_ptr<dartfn_ctl<Op>> ctl_;

  io_env get_env() const noexcept { return io_env{ctl_->op->sched_}; }

  void set_value(std::optional<DartFnReply> reply) && noexcept {
    ctl_->op->post_reply(std::move(reply));
  }

  void set_error(std::exception_ptr ep) && noexcept {
    ctl_->op->post_error(std::move(ep));
  }

  void set_stopped() && noexcept {
    ctl_->op->post_stopped();
  }
};

// Sender that performs a DartFn reverse call: waits for the Dart reply
// (oneshot channel), decodes the payload, and completes with Ret on the io
// thread. Errors (channel closed, Dart-side failure, decode failure) are
// delivered as set_error(std::exception_ptr).
template <typename Ret>
struct dartfn_sender {
  using sender_concept = stdexec::sender_tag;
  using completion_signatures = stdexec::completion_signatures<
    stdexec::set_value_t(Ret),
    stdexec::set_error_t(std::exception_ptr)>;

  using DecodeRet = std::function<Ret(const std::uint8_t*, std::size_t)>;

  // Attributes exposed via get_env(): algorithms probe the completion
  // behavior of their child sender.
  struct attrs {
    constexpr auto query(
        stdexec::__get_completion_behavior_t<stdexec::set_value_t>) const noexcept {
      return stdexec::__completion_behavior::__inline_completion;
    }
    constexpr auto operator==(const attrs&) const noexcept -> bool = default;
  };

  static constexpr auto get_env() noexcept -> attrs { return {}; }

  co::oneshot::Receiver<DartFnReply> rx_;
  const AsioScheduler* sched_;
  DecodeRet decode_;

  template <stdexec::receiver Rcvr>
  struct opstate {
    using operation_state_concept = stdexec::operation_state_tag;

    using inner_rcvr_t = dartfn_inner_receiver<opstate>;
    using inner_op_t = stdexec::connect_result_t<
      co::oneshot::Receiver<DartFnReply>, inner_rcvr_t>;

    const AsioScheduler* sched_;
    Rcvr rcvr_;
    DecodeRet decode_;
    std::shared_ptr<dartfn_ctl<opstate>> ctl_;
    inner_op_t inner_;

    opstate(const AsioScheduler* sched, co::oneshot::Receiver<DartFnReply> rx,
            Rcvr rcvr, DecodeRet decode)
      : sched_(sched),
        rcvr_(std::move(rcvr)),
        decode_(std::move(decode)),
        ctl_(std::make_shared<dartfn_ctl<opstate>>()),
        inner_(stdexec::connect(std::move(rx), inner_rcvr_t{ctl_})) {
      ctl_->op = this;
    }

    opstate(opstate&& o) noexcept
      : sched_(o.sched_),
        rcvr_(std::move(o.rcvr_)),
        decode_(std::move(o.decode_)),
        ctl_(std::move(o.ctl_)),
        inner_(std::move(o.inner_)) {
      ctl_->op = this;
    }

    opstate(const opstate&) = delete;
    opstate& operator=(const opstate&) = delete;
    opstate& operator=(opstate&&) = delete;

    void start() noexcept { stdexec::start(inner_); }

    void post_reply(std::optional<DartFnReply> reply) {
      try {
        asio::post(sched_->io(), [this, reply = std::move(reply)]() mutable {
          try {
            if (!reply) {
              throw std::runtime_error("DartFn: channel closed");
            }
            if (!reply->ok) {
              throw std::runtime_error(reply->error.empty() ? "DartFn failed"
                                                            : reply->error);
            }
            stdexec::set_value(std::move(rcvr_),
                               decode_(reply->payload.data(), reply->payload.size()));
          } catch (...) {
            stdexec::set_error(std::move(rcvr_), std::current_exception());
          }
        });
      } catch (...) {
        stdexec::set_error(std::move(rcvr_),
                           std::make_exception_ptr(std::bad_alloc()));
      }
    }

    void post_error(std::exception_ptr ep) {
      try {
        asio::post(sched_->io(), [this, ep]() mutable {
          stdexec::set_error(std::move(rcvr_), ep);
        });
      } catch (...) {
        stdexec::set_error(std::move(rcvr_),
                           std::make_exception_ptr(std::bad_alloc()));
      }
    }

    void post_stopped() {
      try {
        asio::post(sched_->io(), [this]() mutable {
          stdexec::set_error(std::move(rcvr_),
                             std::make_exception_ptr(
                                 std::runtime_error("DartFn stopped")));
        });
      } catch (...) {
        stdexec::set_error(std::move(rcvr_),
                           std::make_exception_ptr(std::bad_alloc()));
      }
    }
  };

  template <stdexec::receiver Rcvr>
  opstate<Rcvr> connect(Rcvr rcvr) && {
    return opstate<Rcvr>(sched_, std::move(rx_), std::move(rcvr),
                         std::move(decode_));
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
//   auto r = dcb::sync_wait(dcb::spawn(fn(args...)));
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
  //   auto r = dcb::sync_wait(dcb::spawn(fn(args...)));
  detail::dartfn_sender<Ret> operator()(const Args&... args) const {
    if (!session_) {
      throw std::runtime_error("DartFn: empty");
    }
    ByteWriter w;
    encode_(w, args...);
    auto* sched = Runtime::instance().scheduler();
    if (!sched) {
      throw std::runtime_error("runtime scheduler missing");
    }
    auto rx =
        session_->invoke_dart_fn_async(generation_, fn_id_, w.raw());
    return detail::dartfn_sender<Ret>{std::move(rx), sched, decode_};
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
