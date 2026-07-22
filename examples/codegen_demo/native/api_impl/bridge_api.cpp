#include "bridge_api.h"

#include <chrono>
#include <thread>

namespace demo::api {

std::int32_t bridge_version() { return 42; }

async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b) {
  co_return a + b;
}

std::string sleep_greeting(std::string name) {
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return std::string("hello, ") + name;
}

}  // namespace demo::api
