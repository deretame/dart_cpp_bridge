#include "dart_cpp_bridge/ffi.h"

#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/runtime.hpp"

#include "dart_api_dl.h"

#include <async_simple/coro/Lazy.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace dcb {
namespace demo {
void dispatch_request(Session* session, const std::uint8_t* data, std::size_t len);
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

}  // namespace

extern "C" {

DCB_API intptr_t dcb_init_dart_api(void* initialize_api_dl_data) {
  return Dart_InitializeApiDL(initialize_api_dl_data);
}

DCB_API void dcb_init(int64_t reply_native_port) {
  auto& rt = dcb::Runtime::instance();
  rt.start();
  rt.set_dart_post(&dart_post_impl, nullptr);
  dcb::global_session().bind_reply_port(reply_native_port);
}

DCB_API void dcb_dispose(void) { dcb::global_session().dispose(); }

DCB_API void dcb_shutdown(void) {
  dcb::global_session().dispose();
  dcb::Runtime::instance().set_dart_post(nullptr, nullptr);
  dcb::Runtime::instance().stop();
}

DCB_API uint8_t* dcb_invoke_sync(const uint8_t* req, size_t req_len, size_t* out_len,
                                 char** error_out) {
  if (error_out) {
    *error_out = nullptr;
  }
  try {
    dcb::Runtime::instance().ensure_running();
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

DCB_API void dcb_invoke_async(const uint8_t* req, size_t req_len) {
  if (!dcb::Runtime::instance().running()) {
    return;
  }
  std::vector<std::uint8_t> copy(req, req + req_len);
  dcb::Runtime::instance().spawn_on_asio([copy = std::move(copy)]() -> async_simple::coro::Lazy<> {
    dcb::demo::dispatch_request(&dcb::global_session(), copy.data(), copy.size());
    co_return;
  });
}

DCB_API void dcb_stream_close(uint64_t stream_id) {
  dcb::global_session().set_stream_open(stream_id, false);
}

DCB_API void dcb_free(void* p) { std::free(p); }

}  // extern "C"
