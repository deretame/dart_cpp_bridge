// Stub for codegen parsing only — the real header ships with standalone asio
// (fetched via CMake FetchContent). Codegen only needs the class template name;
// the real asio header chain is not parsed.
#pragma once

#include <asio/io_context.hpp>

namespace asio {

template <typename Executor>
class executor_work_guard {
 public:
  explicit executor_work_guard(const Executor&) {}
};

}  // namespace asio
