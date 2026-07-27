#pragma once
// Parse-only stub for codegen (real build uses FetchContent async-simple).
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace async_simple {

// Forward declarations for types used by AsioExecutor.
class IOExecutor;
class Slot;

// Context is an opaque handle passed to checkin().
using Context = void*;

// Options controlling how a task is scheduled back into the executor.
struct ScheduleOptions {
  bool prompt = true;  // true = dispatch (immediate), false = post (deferred)
};

// Snapshot of executor statistics.
struct ExecutorStat {
  size_t pendingTaskCount = 0;
};

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

  // Core scheduling.
  virtual bool schedule(Func func) = 0;
  virtual bool schedule(Func func, uint64_t schedule_info) {
    return schedule(std::move(func));
  }

  // Context-aware scheduling (used by RescheduleLazy).
  virtual bool checkin(Func func, Context /*ctx*/, ScheduleOptions /*opts*/) {
    return schedule(std::move(func));
  }
  virtual void* checkout() { return nullptr; }

  // Statistics and introspection.
  virtual ExecutorStat stat() const { return {}; }
  virtual IOExecutor* getIOExecutor() { return nullptr; }
  virtual bool currentThreadInExecutor() const { return false; }
  virtual size_t currentContextId() const { return 0; }

 protected:
  // Timer-based scheduling (used by after() / coro::sleep).
  virtual void schedule(Func func, Duration dur, uint64_t /*schedule_info*/,
                        Slot* /*slot*/) {
    // Default: ignore duration and run immediately.
    schedule(std::move(func));
  }
};

// Placeholder for IOExecutor (not used by the bridge).
class IOExecutor {
 public:
  virtual ~IOExecutor() = default;
};

// Placeholder for Slot (cancellation support, not used yet).
class Slot {
 public:
  virtual ~Slot() = default;
};

}  // namespace async_simple
