// Stub for codegen parsing only — the real header ships with Boost.Asio.
#pragma once

namespace boost::asio {
template <typename Executor, typename CompletionToken>
void dispatch(Executor&&, CompletionToken&&) {}
}  // namespace boost::asio
