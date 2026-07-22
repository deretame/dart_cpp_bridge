#include "dart_cpp_bridge/ffi.h"

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

#include "dart_api_dl.h"

#include <async_simple/coro/Lazy.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace dcb {
namespace demo {
void dispatch_request(std::shared_ptr<Session> session, const std::uint8_t* data, std::size_t len);
std::vector<std::uint8_t> dispatch_sync(const std::uint8_t* data, std::size_t len);
}  // namespace demo
}  // namespace dcb

namespace {

void dart_post_impl(std::int64_t port, const std::uint8_t* data, std::size_t len, void*) {
  if (port == 0 || data == nullptr) {
    return;
  }
  Dart_CObject obj{};
  obj.type = Dart_CObject_kTypedData;
  obj.value.as_typed_data.type = Dart_TypedData_kUint8;
  obj.value.as_typed_data.length = static_cast<intptr_t>(len);
  obj.value.as_typed_data.values = const_cast<uint8_t*>(data);
  Dart_PostCObject_DL(static_cast<Dart_Port_DL>(port), &obj);
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
  if (token == nullptr) {
    return;
  }
  const auto id = *static_cast<uint64_t*>(token);
  std::free(token);
  dcb::SessionRegistry::instance().close(id);
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
    auto out = dcb::demo::dispatch_sync(req, req_len);
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
  if (!session || !dcb::Runtime::instance().running()) {
    return;
  }
  std::vector<std::uint8_t> copy(req, req + req_len);
  dcb::Runtime::instance().spawn_on_asio(
      [session = std::move(session), copy = std::move(copy)]() -> async_simple::coro::Lazy<> {
        dcb::demo::dispatch_request(session, copy.data(), copy.size());
        co_return;
      });
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

DCB_API void dcb_free(void* p) { std::free(p); }

}  // extern "C"
