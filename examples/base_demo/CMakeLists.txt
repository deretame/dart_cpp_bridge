# =============================================================================
# base_demo — 手写 wire dispatch 示例 + C++ smoke test
# =============================================================================
#
# 构建产物：
#   - dart_cpp_bridge.dll : 包含基础运行时 + 手写 demo_api wire dispatch
#   - dcb_smoke.exe       : 纯 C++ smoke test（无 Dart VM）
#
# Dart 测试（dart/test/）加载本目录构建的 DLL。
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(dcb_base_demo LANGUAGES C CXX)

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

set(DCB_DART_API_DIR ${DCB_ROOT}/third_party/dart_api)
if(NOT EXISTS "${DCB_DART_API_DIR}/dart_api_dl.c")
  message(FATAL_ERROR "Missing third_party/dart_api. Run: cmake -P ../../dart/fetch_dart_api.cmake")
endif()

# --- 动态库（Dart 测试加载此 DLL） ---
add_library(dart_cpp_bridge SHARED
  ${DCB_ROOT}/src/runtime/runtime.cpp
  ${DCB_ROOT}/src/runtime/object_handle.cpp
  ${DCB_ROOT}/src/ffi_entry.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
  demo_api.cpp
)

target_include_directories(dart_cpp_bridge
  PRIVATE
    ${DCB_ROOT}/include
    ${DCB_DART_API_DIR}
)

target_compile_definitions(dart_cpp_bridge PRIVATE DART_CPP_BRIDGE_BUILD DCB_REGISTER_DISPATCH)
target_link_libraries(dart_cpp_bridge PUBLIC asio_iface async_simple_iface)

if(WIN32)
  target_compile_definitions(dart_cpp_bridge PRIVATE _WIN32_WINNT=0x0A00)
  target_compile_options(dart_cpp_bridge PRIVATE /utf-8)
endif()

# --- C++ smoke test（无 Dart VM） ---
add_executable(dcb_smoke
  smoke_main.cpp
  ${DCB_ROOT}/src/runtime/runtime.cpp
  ${DCB_ROOT}/src/runtime/object_handle.cpp
  demo_api.cpp
)

target_include_directories(dcb_smoke PRIVATE
  ${DCB_ROOT}/include
  ${DCB_DART_API_DIR}
)

target_link_libraries(dcb_smoke PRIVATE asio_iface async_simple_iface)

if(WIN32)
  target_compile_definitions(dcb_smoke PRIVATE _WIN32_WINNT=0x0A00)
  target_compile_options(dcb_smoke PRIVATE /utf-8)
endif()
