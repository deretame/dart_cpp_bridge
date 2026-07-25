// foreign_runtime.cpp — C API 实现：外部运行时注册表 + ForeignExecutor 管理。

#include "dart_cpp_bridge/foreign_runtime.h"

#include "dart_cpp_bridge/foreign_executor.hpp"
#include "dart_cpp_bridge/runtime.hpp"

#include <asio/post.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dcb {
namespace {

struct ForeignEntry {
  std::unique_ptr<ForeignExecutor> executor;
};

std::mutex g_foreign_mu;
std::unordered_map<std::uint32_t, ForeignEntry> g_foreign_registry;
std::uint32_t g_foreign_next_id = 1;

}  // namespace
}  // namespace dcb

// ─── C API 实现 ──────────────────────────────────────────────────────────────

extern "C" {

DCB_API uint32_t dcb_foreign_register(const char* name, dcb_schedule_fn schedule_fn, void* ctx) {
  if (!schedule_fn) return 0;

  std::lock_guard lock(dcb::g_foreign_mu);
  auto id = dcb::g_foreign_next_id++;
  auto executor = std::make_unique<dcb::ForeignExecutor>(
      name ? name : "foreign", schedule_fn, ctx);

  dcb::g_foreign_registry[id] = dcb::ForeignEntry{std::move(executor)};
  return id;
}

DCB_API void dcb_foreign_unregister(uint32_t runtime_id) {
  std::lock_guard lock(dcb::g_foreign_mu);
  auto it = dcb::g_foreign_registry.find(runtime_id);
  if (it == dcb::g_foreign_registry.end()) return;

  // 标记失效（之后 schedule 返回 false）
  it->second.executor->deactivate();
  dcb::g_foreign_registry.erase(it);
}

DCB_API void dcb_post_to_bridge(void (*fn)(void*), void* userdata) {
  if (!fn) return;
  auto& rt = dcb::Runtime::instance();
  if (!rt.running()) return;

  asio::post(rt.io(), [fn, userdata]() {
    fn(userdata);
  });
}

DCB_API void* dcb_foreign_executor(uint32_t runtime_id) {
  std::lock_guard lock(dcb::g_foreign_mu);
  auto it = dcb::g_foreign_registry.find(runtime_id);
  if (it == dcb::g_foreign_registry.end()) return nullptr;
  return static_cast<void*>(it->second.executor.get());
}

}  // extern "C"
