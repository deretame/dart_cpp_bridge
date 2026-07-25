#include "dart_cpp_bridge/object_handle.hpp"

#include <cassert>

namespace dcb {

ObjectHandleRegistry& ObjectHandleRegistry::instance() {
  static ObjectHandleRegistry registry;
  return registry;
}

ObjectHandleRegistry::Handle ObjectHandleRegistry::insert(
    std::uint64_t session_id, std::shared_ptr<void> obj, DropFn drop) {
  assert(session_id != 0 && session_id <= 0xFFFFFFFFULL && "session_id must fit in 32 bits");
  auto store = session_store(session_id);
  Handle local = 0;
  {
    std::lock_guard<std::mutex> lock(store->mu);
    local = store->next_handle++;
    store->objects.emplace(local, std::make_pair(std::move(obj), std::move(drop)));
  }
  return (session_id << kSessionShift) | local;
}

std::shared_ptr<void> ObjectHandleRegistry::get(Handle handle) const {
  auto store = session_store_for_handle(handle);
  if (!store) {
    return nullptr;
  }
  const auto local = handle & kLocalMask;
  std::lock_guard<std::mutex> lock(store->mu);
  auto it = store->objects.find(local);
  if (it == store->objects.end()) {
    return nullptr;
  }
  return it->second.first;
}

void ObjectHandleRegistry::drop(Handle handle) {
  auto store = session_store_for_handle(handle);
  if (!store) {
    return;
  }
  const auto local = handle & kLocalMask;
  std::pair<std::shared_ptr<void>, DropFn> entry;
  {
    std::lock_guard<std::mutex> lock(store->mu);
    auto it = store->objects.find(local);
    if (it == store->objects.end()) {
      return;
    }
    entry = std::move(it->second);
    store->objects.erase(it);
  }
  if (entry.second) {
    entry.second(entry.first);
  }
}

void ObjectHandleRegistry::drop_all(std::uint64_t session_id) {
  std::shared_ptr<SessionStore> store;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      return;
    }
    store = std::move(it->second);
    sessions_.erase(it);
  }
  if (!store) {
    return;
  }
  std::unordered_map<Handle, std::pair<std::shared_ptr<void>, DropFn>> objects;
  {
    std::lock_guard<std::mutex> lock(store->mu);
    objects = std::move(store->objects);
  }
  for (auto& kv : objects) {
    if (kv.second.second) {
      kv.second.second(kv.second.first);
    }
  }
}

std::shared_ptr<ObjectHandleRegistry::SessionStore> ObjectHandleRegistry::session_store(
    std::uint64_t session_id) {
  std::lock_guard<std::mutex> lock(mu_);
  auto& store = sessions_[session_id];
  if (!store) {
    store = std::make_shared<SessionStore>();
  }
  return store;
}

std::shared_ptr<ObjectHandleRegistry::SessionStore> ObjectHandleRegistry::session_store_for_handle(
    Handle handle) const {
  const auto session_id = handle >> kSessionShift;
  if (session_id == 0) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(mu_);
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return it->second;
}

}  // namespace dcb
