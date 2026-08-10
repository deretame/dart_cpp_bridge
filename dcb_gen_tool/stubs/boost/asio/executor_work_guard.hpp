// Stub for codegen parsing only — the real header ships with Boost.Asio
// (used when DCB_USE_BOOST_ASIO is defined). Codegen only needs the class
// template name; the real Boost.Asio header chain is not parsed.
#pragma once

#include <boost/asio/io_context.hpp>

namespace boost::asio {

template <typename Executor>
class executor_work_guard {
 public:
  explicit executor_work_guard(const Executor&) {}
};

}  // namespace boost::asio
