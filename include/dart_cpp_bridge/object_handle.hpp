#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace dcb {

// Minimal object handle registry for exported C++ classes.
// Used by generated code to keep opaque C++ objects alive across Dart FFI calls.
//
// Design notes:
// - Handles are per-process in this minimal implementation. The long-term design
//   document calls for per-Session handles (no cross-Isolate sharing), but a
//   process-wide registry is sufficient for the hand-written Counter fixture.
// - Objects are stored as shared_ptr<void> with a custom drop function so the
//   registry can hold heterogeneous types without a common base class.
class ObjectHandleRegistry {
 public:
  using Handle = std::uint64_t;
  using DropFn = std::function<void(std::shared_ptr<void>&)>;

  static ObjectHandleRegistry& instance();

  // Store an object and return a handle. The drop function is invoked when the
  // handle is explicitly dropped (usually from Dart NativeFinalizer).
  Handle insert(std::shared_ptr<void> obj, DropFn drop);

  // Retrieve the object or nullptr if the handle is invalid/dropped.
  std::shared_ptr<void> get(Handle handle) const;

  // Drop the object associated with the handle. Idempotent.
  void drop(Handle handle);

 private:
  mutable std::mutex mu_;
  std::unordered_map<Handle, std::pair<std::shared_ptr<void>, DropFn>> objects_;
  Handle next_handle_ = 1;
};

}  // namespace dcb
