#pragma once
// Parse-only stub for codegen (real build uses FetchContent async-simple).
#include <chrono>
#include <cstdint>
#include <functional>

namespace async_simple {

class Executor {
 public:
  using Func = std::function<void()>;
  using Duration = std::chrono::nanoseconds;

  enum class Priority : uint8_t {
    YIELD = 0,
    DEFAULT = 1,
    HIGH = 2,
  };

  virtual ~Executor() = default;
  virtual bool schedule(Func func) = 0;
  virtual bool schedule(Func func, uint64_t schedule_info) { return schedule(std::move(func)); }
  virtual bool checkin(Func func, void* ctx) { return schedule(std::move(func)); }
  virtual void* checkout() { return nullptr; }
  virtual bool currentThreadInExecutor() const { return false; }
  virtual size_t currentContextId() const { return 0; }

 private:
  virtual void schedule(Func func, Duration dur) {}
};

}  // namespace async_simple
