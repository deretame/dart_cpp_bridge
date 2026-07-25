#pragma once

// Dispatch registration — decouples the base FFI entry from user wire code.
//
// User projects (hand-written or codegen-generated) implement dispatch_request
// and dispatch_sync, then register them via set_dispatch() before any Dart
// invocation arrives. Typically done with a file-scope static initializer:
//
//   namespace { const bool _ok = [] { dcb::set_dispatch(&my_async, &my_sync); return true; }(); }

#include <cstdint>
#include <memory>
#include <vector>

namespace dcb {

class Session;

/// Async dispatch: parse frame, route method, post response via session.
using DispatchRequestFn = void (*)(std::shared_ptr<Session>, std::uint64_t,
                                   const std::uint8_t*, std::size_t);

/// Sync dispatch: parse frame, route method, return response bytes.
using DispatchSyncFn = std::vector<std::uint8_t> (*)(std::uint64_t,
                                                     const std::uint8_t*, std::size_t);

/// Register user dispatch functions. Must be called before any dcb_invoke_*.
void set_dispatch(DispatchRequestFn async_fn, DispatchSyncFn sync_fn);

/// Access registered dispatch (throws if not yet registered).
DispatchRequestFn dispatch_request_fn();
DispatchSyncFn dispatch_sync_fn();

}  // namespace dcb
