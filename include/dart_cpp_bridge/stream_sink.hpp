#pragma once

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dcb {

template <class EncodeFn>
class StreamSink {
 public:
  StreamSink(Session* session, std::uint64_t stream_id, std::uint64_t generation,
             std::uint32_t method_id, EncodeFn encode)
      : state_(std::make_shared<State>()) {
    state_->session = session;
    state_->stream_id = stream_id;
    state_->generation = generation;
    state_->method_id = method_id;
    state_->encode = std::move(encode);
    session->set_stream_open(stream_id, true);
  }

  template <class T>
  void add(const T& item) {
    auto st = state_;
    if (!st) {
      return;
    }
    std::lock_guard lock(st->mu);
    if (st->closed) {
      return;
    }
    if (!st->session->alive(st->generation) || !st->session->stream_open(st->stream_id)) {
      st->closed = true;
      return;
    }
    try {
      auto payload = st->encode(item);
      auto frame = make_frame(MsgType::kStreamData, st->stream_id, st->method_id, payload);
      st->session->try_post(st->generation, frame);
    } catch (...) {
      post_err_locked(st, "encode failed");
    }
  }

  void end() {
    auto st = state_;
    if (!st) {
      return;
    }
    std::lock_guard lock(st->mu);
    if (st->closed) {
      return;
    }
    st->closed = true;
    if (!st->session->alive(st->generation)) {
      return;
    }
    auto frame = make_frame(MsgType::kStreamEnd, st->stream_id, st->method_id, {});
    st->session->try_post(st->generation, frame);
    st->session->set_stream_open(st->stream_id, false);
  }

  void error(const std::string& message) {
    auto st = state_;
    if (!st) {
      return;
    }
    std::lock_guard lock(st->mu);
    post_err_locked(st, message);
  }

 private:
  struct State {
    Session* session{nullptr};
    std::uint64_t stream_id{0};
    std::uint64_t generation{0};
    std::uint32_t method_id{0};
    EncodeFn encode{};
    std::mutex mu;
    bool closed{false};
  };

  static void post_err_locked(const std::shared_ptr<State>& st, const std::string& message) {
    if (st->closed) {
      return;
    }
    st->closed = true;
    if (!st->session->alive(st->generation)) {
      return;
    }
    ByteWriter w;
    w.i32(1);
    w.str(message);
    auto frame = make_frame(MsgType::kStreamErr, st->stream_id, st->method_id, w.raw());
    st->session->try_post(st->generation, frame);
    st->session->set_stream_open(st->stream_id, false);
  }

  std::shared_ptr<State> state_;
};

inline auto make_i32_sink(Session* session, std::uint64_t stream_id, std::uint64_t generation,
                          std::uint32_t method_id) {
  return StreamSink(session, stream_id, generation, method_id, [](std::int32_t v) {
    ByteWriter w;
    w.i32(v);
    return w.raw();
  });
}

}  // namespace dcb
