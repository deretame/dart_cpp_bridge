#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace dcb {

// Per-Session object handle registry for exported C++ classes.
// Used by generated code to keep opaque C++ objects alive across Dart FFI calls.
//
// Design notes:
// - Handles are scoped to a Session. A full handle encodes the session id in
//   the high 32 bits and a per-session local handle in the low 32 bits.
// - Opaque objects cannot be shared across Isolates: a handle created in one
//   Session is invalid in another Session's registry.
// - Objects are stored as shared_ptr<void> with a custom drop function so the
//   registry can hold heterogeneous types without a common base class.
// - When a Session is closed, drop_all(session_id) releases all objects owned
//   by that session.
// - Pure data classes/structs are NOT registered here; they are encoded by
//   value and can cross Isolate boundaries freely.
class ObjectHandleRegistry {
 public:
  using Handle = std::uint64_t;
  using DropFn = std::function<void(std::shared_ptr<void>&)>;

  static ObjectHandleRegistry& instance();

  // Store an object for [session_id] and return a full handle. The drop
  // function is invoked when the handle is explicitly dropped (usually from
  // Dart NativeFinalizer) or when the session is closed.
  Handle insert(std::uint64_t session_id, std::shared_ptr<void> obj, DropFn drop);

  // Retrieve the object for a full [handle], or nullptr if the handle is
  // invalid, dropped, or belongs to a different session.
  std::shared_ptr<void> get(Handle handle) const;

  // Drop the object for a full [handle]. Idempotent.
  void drop(Handle handle);

  // Drop all objects belonging to [session_id]. Called when the session closes.
  void drop_all(std::uint64_t session_id);

 private:
  struct SessionStore {
    std::mutex mu;
    std::unordered_map<Handle, std::pair<std::shared_ptr<void>, DropFn>> objects;
    Handle next_handle = 1;
  };

  static constexpr std::uint64_t kLocalMask = 0xFFFFFFFFULL;
  static constexpr int kSessionShift = 32;

  std::shared_ptr<SessionStore> session_store(std::uint64_t session_id);
  std::shared_ptr<SessionStore> session_store_for_handle(Handle handle) const;

  mutable std::mutex mu_;
  std::unordered_map<std::uint64_t, std::shared_ptr<SessionStore>> sessions_;
};

}  // namespace dcb
