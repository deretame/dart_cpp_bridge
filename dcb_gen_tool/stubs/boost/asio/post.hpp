// Stub for codegen parsing only — the real header ships with Boost.Asio.
#pragma once

namespace boost::asio {
template <typename CompletionToken>
void post(CompletionToken&&) {}
}  // namespace boost::asio
