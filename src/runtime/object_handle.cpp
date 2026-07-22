#include "dart_cpp_bridge/object_handle.hpp"

namespace dcb {

ObjectHandleRegistry& ObjectHandleRegistry::instance() {
  static ObjectHandleRegistry registry;
  return registry;
}

ObjectHandleRegistry::Handle ObjectHandleRegistry::insert(std::shared_ptr<void> obj, DropFn drop) {
  std::lock_guard<std::mutex> lock(mu_);
  Handle h = next_handle_++;
  objects_.emplace(h, std::make_pair(std::move(obj), std::move(drop)));
  return h;
}

std::shared_ptr<void> ObjectHandleRegistry::get(Handle handle) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = objects_.find(handle);
  if (it == objects_.end()) return nullptr;
  return it->second.first;
}

void ObjectHandleRegistry::drop(Handle handle) {
  std::pair<std::shared_ptr<void>, DropFn> entry;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = objects_.find(handle);
    if (it == objects_.end()) return;
    entry = std::move(it->second);
    objects_.erase(it);
  }
  if (entry.second) {
    entry.second(entry.first);
  }
}

}  // namespace dcb
