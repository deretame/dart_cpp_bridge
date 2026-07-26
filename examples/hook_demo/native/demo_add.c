// Minimal native functions for the hook_demo Native Assets pipeline check.
// No dart_cpp_bridge runtime involvement; pure exported C functions.

#include <stdint.h>

#if defined(_WIN32)
#define HOOK_DEMO_EXPORT __declspec(dllexport)
#else
#define HOOK_DEMO_EXPORT __attribute__((visibility("default")))
#endif

HOOK_DEMO_EXPORT int32_t hook_demo_add(int32_t a, int32_t b) {
  return a + b;
}

HOOK_DEMO_EXPORT int32_t hook_demo_sub(int32_t a, int32_t b) {
  return a - b;
}

HOOK_DEMO_EXPORT int32_t hook_demo_mul(int32_t a, int32_t b) {
  return a * b;
}
