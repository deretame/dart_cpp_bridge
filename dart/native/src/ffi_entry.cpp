#include "dart_cpp_bridge/ffi.h"

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/dispatch.hpp"
#include "dart_cpp_bridge/error_config.hpp"
#include "dart_cpp_bridge/object_handle.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

#include "dart_api_dl.h"

#include <stdexec/execution.hpp>
#include <exec/start_detached.hpp>

#include <cstdlib>
#include <cstring>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace dcb {
namespace {
struct DispatchState {
  DispatchRequestFn request = nullptr;
  DispatchSyncFn sync = nullptr;
};
DispatchState& dispatch_state() {
  static DispatchState s;
  return s;
}
}  // namespace

void set_dispatch(DispatchRequestFn async_fn, DispatchSyncFn sync_fn) {
  auto& s = dispatch_state();
  s.request = async_fn;
  s.sync = sync_fn;
}

DispatchRequestFn dispatch_request_fn() {
  auto fn = dispatch_state().request;
  if (!fn) throw std::runtime_error("dcb: dispatch not registered");
  return fn;
}

DispatchSyncFn dispatch_sync_fn() {
  auto fn = dispatch_state().sync;
  if (!fn) throw std::runtime_error("dcb: dispatch not registered");
  return fn;
}

}  // namespace dcb

namespace {

void free_posted_data(void* /*isolate_callback_data*/, void* peer) { std::free(peer); }

void dart_post_impl(std::int64_t port, const std::uint8_t* data, std::size_t len, void*) {
  if (port == 0 || data == nullptr) {
    return;
  }
  // Copy the frame so callers do not have to keep local vectors alive while
  // Dart processes the message. Use external typed data so Dart owns and frees
  // the copy via the finalizer.
  auto* copy = static_cast<uint8_t*>(std::malloc(len ? len : 1));
  if (!copy) {
    return;
  }
  if (len > 0) {
    std::memcpy(copy, data, len);
  }

  Dart_CObject obj{};
  obj.type = Dart_CObject_kExternalTypedData;
  obj.value.as_external_typed_data.type = Dart_TypedData_kUint8;
  obj.value.as_external_typed_data.length = static_cast<intptr_t>(len);
  obj.value.as_external_typed_data.data = copy;
  obj.value.as_external_typed_data.peer = copy;
  obj.value.as_external_typed_data.callback = free_posted_data;
  if (!Dart_PostCObject_DL(static_cast<Dart_Port_DL>(port), &obj)) {
    // Port closed / message dropped; free the copy ourselves.
    std::free(copy);
  }
}

char* dup_err(const std::string& s) {
  auto* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) {
    return nullptr;
  }
  std::memcpy(p, s.c_str(), s.size() + 1);
  return p;
}

uint8_t* dup_bytes(const std::vector<std::uint8_t>& v, size_t* out_len) {
  auto* p = static_cast<uint8_t*>(std::malloc(v.size() ? v.size() : 1));
  if (!p) {
    return nullptr;
  }
  if (!v.empty()) {
    std::memcpy(p, v.data(), v.size());
  }
  if (out_len) {
    *out_len = v.size();
  }
  return p;
}

void ensure_post_hook() { dcb::Runtime::instance().set_dart_post(&dart_post_impl, nullptr); }

void post_async_error(const std::shared_ptr<dcb::Session>& session,
                      const std::uint8_t* req, std::size_t req_len,
                      const char* message) {
  if (!session || req == nullptr) {
    return;
  }
  try {
    const auto frame = dcb::parse_frame(req, req_len);
    dcb::ByteWriter w;
    w.i32(1);
    w.str(message);
    session->try_post(
        session->generation(),
        dcb::make_frame(dcb::MsgType::kResponseErr, frame.request_id,
                        frame.method_id, w.raw()));
  } catch (...) {
    // The request may itself be malformed or the runtime may already be
    // stopping. There is no safe request id to answer with in that case.
  }
}

}  // namespace

extern "C" {

DCB_API intptr_t dcb_init_dart_api(void* initialize_api_dl_data) {
  return Dart_InitializeApiDL(initialize_api_dl_data);
}

DCB_API uint64_t dcb_session_open(int64_t reply_native_port) {
  try {
    auto& rt = dcb::Runtime::instance();
    rt.start();
    ensure_post_hook();
    return dcb::SessionRegistry::instance().open(reply_native_port);
  } catch (...) {
    return 0;
  }
}

DCB_API void dcb_session_close(uint64_t session_id) {
  dcb::SessionRegistry::instance().close(session_id);
}

DCB_API void dcb_session_finalizer(void* token) {
  try {
    if (token == nullptr) {
      return;
    }
    // The token is the session id encoded as a pointer value; no heap allocation
    // is involved, avoiding cross-module malloc/free mismatches on hot restart.
    const auto id = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(token));
    dcb::SessionRegistry::instance().close(id);
  } catch (const std::exception& e) {
    std::cerr << "[dcb] dcb_session_finalizer failed: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[dcb] dcb_session_finalizer failed: unknown exception" << std::endl;
  }
}

DCB_API void dcb_shutdown(void) {
  dcb::SessionRegistry::instance().close_all();
  dcb::Runtime::instance().set_dart_post(nullptr, nullptr);
  dcb::Runtime::instance().stop();
}

DCB_API uint8_t* dcb_invoke_sync(uint64_t session_id, const uint8_t* req, size_t req_len,
                                 size_t* out_len, char** error_out) {
  if (error_out) {
    *error_out = nullptr;
  }
  try {
    dcb::Runtime::instance().ensure_running();
    if (!dcb::SessionRegistry::instance().get(session_id)) {
      throw std::runtime_error("invalid session");
    }
    auto out = dcb::dispatch_sync_fn()(session_id, req, req_len);
    return dup_bytes(out, out_len);
  } catch (const std::exception& e) {
    if (error_out) {
      *error_out = dup_err(e.what());
    }
    return nullptr;
  } catch (...) {
    if (error_out) {
      *error_out = dup_err("unknown");
    }
    return nullptr;
  }
}

DCB_API void dcb_invoke_async(uint64_t session_id, const uint8_t* req, size_t req_len) {
  auto session = dcb::SessionRegistry::instance().get(session_id);
  if (!session) {
    return;
  }
  if (!dcb::Runtime::instance().running()) {
    post_async_error(session, req, req_len, "runtime stopped");
    return;
  }
  try {
    const auto dispatch = dcb::dispatch_request_fn();
    std::vector<std::uint8_t> copy(req, req + req_len);
    // std::exec style: launch a dispatch chain on the io scheduler
    // (starts-on io; dispatch runs on the io thread). Dispatch implementations
    // catch business exceptions at the wire boundary; this extra guard keeps a
    // bad registration or third-party dispatcher from terminating the process.
    auto sndr = stdexec::just() | stdexec::then(
        [session = std::move(session), copy = std::move(copy), session_id,
         dispatch]() noexcept {
          try {
            dispatch(session, session_id, copy.data(), copy.size());
          } catch (const std::exception& e) {
            post_async_error(session, copy.data(), copy.size(), e.what());
          } catch (...) {
            post_async_error(session, copy.data(), copy.size(), "async dispatch failed");
          }
        });
    auto chain = stdexec::starts_on(*dcb::Runtime::instance().io_scheduler(),
                                    std::move(sndr));
    exec::start_detached(std::move(chain));
  } catch (const std::exception& e) {
    post_async_error(session, req, req_len, e.what());
  } catch (...) {
    post_async_error(session, req, req_len, "async invoke failed");
  }
}

DCB_API void dcb_stream_close(uint64_t session_id, uint64_t stream_id) {
  auto session = dcb::SessionRegistry::instance().get(session_id);
  if (session) {
    session->set_stream_open(stream_id, false);
  }
}

DCB_API void dcb_dart_fn_reply(uint64_t session_id, uint64_t reply_id, uint8_t ok,
                               const uint8_t* payload, size_t payload_len, const char* error_msg) {
  auto session = dcb::SessionRegistry::instance().get(session_id);
  if (!session) {
    return;
  }
  std::vector<std::uint8_t> bytes;
  if (payload != nullptr && payload_len > 0) {
    bytes.assign(payload, payload + payload_len);
  }
  std::string err = error_msg ? error_msg : "";
  session->complete_dart_fn(reply_id, ok != 0, std::move(bytes), std::move(err));
}

DCB_API void dcb_drop_object(uint64_t handle) {
  try {
    dcb::ObjectHandleRegistry::instance().drop(handle);
  } catch (const std::exception& e) {
    std::cerr << "[dcb] dcb_drop_object failed: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[dcb] dcb_drop_object failed: unknown exception" << std::endl;
  }
}

DCB_API void* dcb_object_finalizer_token(uint64_t handle) {
  try {
    return new uint64_t(handle);
  } catch (...) {
    return nullptr;
  }
}

DCB_API void dcb_object_finalizer(void* token) {
  try {
    if (token == nullptr) {
      return;
    }
    const std::unique_ptr<uint64_t> handle(static_cast<uint64_t*>(token));
    dcb::ObjectHandleRegistry::instance().drop(*handle);
  } catch (const std::exception& e) {
    std::cerr << "[dcb] dcb_object_finalizer failed: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "[dcb] dcb_object_finalizer failed: unknown exception" << std::endl;
  }
}

DCB_API void dcb_free(void* p) { std::free(p); }

DCB_API void dcb_set_verbose_errors(uint8_t enabled) {
  dcb::error::verbose().store(enabled != 0, std::memory_order_relaxed);
}

DCB_API void dcb_set_pool_threads(uint32_t n) {
  dcb::Runtime::instance().set_pool_threads(n);
}

DCB_API void* dcb_session_finalizer_ptr(void) {
  return reinterpret_cast<void*>(&dcb_session_finalizer);
}

DCB_API void* dcb_drop_object_ptr(void) {
  return reinterpret_cast<void*>(&dcb_drop_object);
}

DCB_API void* dcb_object_finalizer_ptr(void) {
  return reinterpret_cast<void*>(&dcb_object_finalizer);
}

}  // extern "C"
