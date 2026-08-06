// Stub for codegen parsing only — the real header is
// dart/native/include/dart_cpp_bridge/dart_fn.hpp. Codegen only needs the
// dcb::DartFn template name; its sender machinery is not parsed.
#pragma once

#include <string>

namespace dcb {

template <class Sig>
class DartFn;

template <class R, class... Args>
class DartFn<R(Args...)> {
 public:
  DartFn() = default;

  explicit operator bool() const { return false; }

  // The real class exposes a sender-producing operator(); codegen only
  // inspects the type.
  auto operator()(Args...) const;
};

}  // namespace dcb
