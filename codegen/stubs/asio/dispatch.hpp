#pragma once
// Parse-only stub for codegen.
namespace asio {
template <typename Executor, typename CompletionToken>
void dispatch(Executor&&, CompletionToken&&) {}
}  // namespace asio
