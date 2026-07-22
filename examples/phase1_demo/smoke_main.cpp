#include "dart_cpp_bridge/codec.hpp"
#include "dart_cpp_bridge/runtime.hpp"
#include "dart_cpp_bridge/session.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace dcb::demo {
std::vector<std::uint8_t> dispatch_sync(const std::uint8_t* data, std::size_t len);
void dispatch_request(std::shared_ptr<Session> session, const std::uint8_t* data, std::size_t len);
}  // namespace dcb::demo

int main() {
  using namespace dcb;
  Runtime::instance().start();
  Runtime::instance().set_dart_post(
      [](std::int64_t, const std::uint8_t* data, std::size_t len, void*) {
        try {
          auto h = parse_frame(data, len);
          std::printf("post type=%u req=%llu payload=%zu\n", static_cast<unsigned>(h.type),
                      static_cast<unsigned long long>(h.request_id), h.payload.size());
        } catch (const std::exception& e) {
          std::printf("post parse error: %s\n", e.what());
        }
      },
      nullptr);

  auto sid = SessionRegistry::instance().open(/*reply_port=*/1);
  auto session = SessionRegistry::instance().get(sid);

  {
    auto req =
        make_frame(MsgType::kRequest, 1, static_cast<std::uint32_t>(MethodId::kBridgeVersion), {});
    auto resp = demo::dispatch_sync(req.data(), req.size());
    auto h = parse_frame(resp.data(), resp.size());
    ByteReader r(h.payload.data(), h.payload.size());
    std::printf("version=%d\n", r.i32());
  }

  {
    ByteWriter payload;
    payload.i32(40);
    payload.i32(2);
    auto req =
        make_frame(MsgType::kRequest, 2, static_cast<std::uint32_t>(MethodId::kAdd), payload.raw());
    demo::dispatch_request(session, req.data(), req.size());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  SessionRegistry::instance().close_all();
  Runtime::instance().stop();
  std::printf("smoke ok\n");
  return 0;
}
