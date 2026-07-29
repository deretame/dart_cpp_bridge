# =============================================================================
# foreign_runtime_demo — 外部运行时集成示例（libuv + ForeignExecutor）
# =============================================================================
#
# 演示非 asio 事件循环（libuv）通过 ForeignExecutor C API 接入 bridge
# 的 channel/coroutine 系统，实现跨运行时非阻塞消息传递。
#
# 基础运行时经 add_subdirectory 接入，并以 WHOLE_ARCHIVE 链接
# dart_cpp_bridge::runtime（STATIC 库，强制链入全部对象以保住 dcb_* 导出）。
# foreign_runtime.cpp（ForeignExecutor C API）已包含在基础库中。
# asio / async-simple 的版本由基础库钉死并传递，本文件只需声明 libuv。
#
# 构建前需先运行 codegen：
#   cd ../../codegen
#   dart run bin/codegen.dart scripts/run_codegen.py ../examples/foreign_runtime_demo/dart_cpp_bridge.yaml
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dcb_foreign_runtime_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 基础运行时（asio / async-simple 由此传递） ---
set(DCB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../dart/native")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- libuv (FetchContent) ---
include(FetchContent)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG        v1.48.0
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libuv)

# --- 生成物检查 ---
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/native/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run codegen first:\n"
    "  cd ../../codegen && dart run bin/codegen.dart scripts/run_codegen.py "
    "../examples/foreign_runtime_demo/dart_cpp_bridge.yaml")
endif()

# --- 动态库 / 静态库 ---
# BUILD_SHARED_LIBS 由 hook 传入（ON=动态库，OFF=静态库，默认 ON）。
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library(dcb_foreign_runtime_demo
  native/generated/wire_dispatch.cpp
  native/api_impl/foreign_api.cpp
)

target_include_directories(dcb_foreign_runtime_demo
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/native
    ${CMAKE_CURRENT_SOURCE_DIR}/native/api
    ${CMAKE_CURRENT_SOURCE_DIR}/native/generated
)

target_link_libraries(dcb_foreign_runtime_demo PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime> uv_a)

if(WIN32)
  target_compile_options(dcb_foreign_runtime_demo PRIVATE /utf-8)
endif()
