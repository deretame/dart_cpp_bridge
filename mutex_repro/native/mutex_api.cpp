// 最小复现：一个导出函数，内部使用 std::mutex。
// 用于对比 @Native (hook) 和 DynamicLibrary.open 两条路径。

#include <mutex>

#if defined(_WIN32)
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

static std::mutex g_mutex;
static int g_counter = 0;

extern "C" {

// 加锁 → 自增 → 解锁 → 返回。最简的 mutex 使用。
API int mutex_increment() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return ++g_counter;
}

// 对照组：不使用 mutex 的纯计算。
API int plain_add(int a, int b) {
  return a + b;
}

}  // extern "C"
