#pragma once

#include <atomic>
#include <string>

namespace dcb {
namespace error {

/// Global flag: when enabled (default), error messages are prefixed with the
/// function name that threw. Toggle via dcb_set_verbose_errors() FFI call.
inline std::atomic<bool>& verbose() {
  static std::atomic<bool> v{true};
  return v;
}

/// Format an error message. If verbose mode is on, prepends "[fn_name] ".
inline std::string format(const char* fn_name, const std::string& msg) {
  if (verbose().load(std::memory_order_relaxed) && fn_name && fn_name[0]) {
    std::string out;
    out.reserve(std::char_traits<char>::length(fn_name) + msg.size() + 3);
    out += '[';
    out += fn_name;
    out += "] ";
    out += msg;
    return out;
  }
  return msg;
}

}  // namespace error
}  // namespace dcb
