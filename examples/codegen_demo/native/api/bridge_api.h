#pragma once

#include "dart_cpp_bridge/annotate.h"

#include <async_simple/coro/Lazy.h>

#include <cstdint>
#include <string>

namespace demo::api {

// unmarked — must not appear in IR
inline std::int32_t internal_helper() { return -1; }

// sync → Dart: int bridgeVersion()
BRIDGE_SYNC
std::int32_t bridge_version();

// async → Dart: Future<int> add(int a, int b)
BRIDGE_ASYNC
async_simple::coro::Lazy<std::int32_t> add(std::int32_t a, std::int32_t b);

// normal (pool) → Dart: Future<String> sleepGreeting(String name)
BRIDGE_NORMAL
std::string sleep_greeting(std::string name);

}  // namespace demo::api
