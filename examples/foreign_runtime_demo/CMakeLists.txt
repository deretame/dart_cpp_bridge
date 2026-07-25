# =============================================================================
# foreign_runtime_demo — 外部运行时集成示例（libuv + ForeignExecutor）
# =============================================================================
#
# 演示非 asio 事件循环（libuv）通过 ForeignExecutor C API 接入 bridge
# 的 channel/coroutine 系统，实现跨运行时非阻塞消息传递。
#
# 构建前需先运行 codegen：
#   cd ../../codegen
#   dart run bin/codegen.dart scripts/run_codegen.py ../examples/foreign_runtime_demo/dart_cpp_bridge.yaml
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(dcb_foreign_runtime_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 依赖解析 ---
set(DCB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../dart/native")
if(NOT DCB_DEPS_DIR)
  set(DCB_DEPS_DIR "${DCB_ROOT}/build/_deps")
endif()

set(_asio_src "${DCB_DEPS_DIR}/asio-src")
set(_as_src "${DCB_DEPS_DIR}/async_simple-src")

if(EXISTS "${_asio_src}/asio/include/asio.hpp")
  message(STATUS "Using asio from ${_asio_src}")
  add_library(asio_iface INTERFACE)
  target_include_directories(asio_iface INTERFACE "${_asio_src}/asio/include")
  target_compile_definitions(asio_iface INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
else()
  message(STATUS "asio not in DCB_DEPS_DIR; FetchContent (needs network)")
  include(FetchContent)
  FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(asio)
  add_library(asio_iface INTERFACE)
  target_include_directories(asio_iface INTERFACE ${asio_SOURCE_DIR}/asio/include)
  target_compile_definitions(asio_iface INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
endif()

if(EXISTS "${_as_src}/async_simple/coro/Lazy.h")
  message(STATUS "Using async-simple from ${_as_src}")
  add_library(async_simple_iface INTERFACE)
  target_include_directories(async_simple_iface INTERFACE "${_as_src}")
else()
  message(STATUS "async-simple not in DCB_DEPS_DIR; FetchContent (needs network)")
  include(FetchContent)
  set(ASYNC_SIMPLE_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(ASYNC_SIMPLE_BUILD_DEMO_EXAMPLE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    async_simple
    GIT_REPOSITORY https://github.com/alibaba/async-simple.git
    GIT_TAG        1.4
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES  ""
  )
  FetchContent_GetProperties(async_simple)
  if(NOT async_simple_POPULATED)
    FetchContent_Populate(async_simple)
  endif()
  add_library(async_simple_iface INTERFACE)
  target_include_directories(async_simple_iface INTERFACE ${async_simple_SOURCE_DIR})
endif()

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

# --- Dart API ---
set(DCB_DART_API_DIR ${DCB_ROOT}/third_party/dart_api)
if(NOT EXISTS "${DCB_DART_API_DIR}/dart_api_dl.c")
  message(FATAL_ERROR "Missing third_party/dart_api. Run: cmake -P ../../dart/fetch_dart_api.cmake")
endif()

# --- 生成物检查 ---
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/native/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run codegen first:\n"
    "  cd ../../codegen && dart run bin/codegen.dart scripts/run_codegen.py "
    "../examples/foreign_runtime_demo/dart_cpp_bridge.yaml")
endif()

# --- 动态库 ---
add_library(dart_cpp_bridge SHARED
  ${DCB_ROOT}/src/runtime/runtime.cpp
  ${DCB_ROOT}/src/runtime/object_handle.cpp
  ${DCB_ROOT}/src/ffi_entry.cpp
  ${DCB_ROOT}/src/foreign_runtime.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
  native/generated/wire_dispatch.cpp
  native/api_impl/foreign_api.cpp
)

target_include_directories(dart_cpp_bridge
  PRIVATE
    ${DCB_ROOT}/include
    ${DCB_DART_API_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/native
    ${CMAKE_CURRENT_SOURCE_DIR}/native/api
    ${CMAKE_CURRENT_SOURCE_DIR}/native/generated
)

target_compile_definitions(dart_cpp_bridge PRIVATE DART_CPP_BRIDGE_BUILD DCB_REGISTER_DISPATCH)
target_link_libraries(dart_cpp_bridge PUBLIC asio_iface async_simple_iface uv_a)

if(WIN32)
  target_compile_definitions(dart_cpp_bridge PRIVATE _WIN32_WINNT=0x0A00)
  target_compile_options(dart_cpp_bridge PRIVATE /utf-8)
endif()
