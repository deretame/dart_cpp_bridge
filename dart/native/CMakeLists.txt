# =============================================================================
# dart_cpp_bridge — 基础运行时库
# =============================================================================
#
# 本目录是基础运行时库。下游项目通过 add_subdirectory / FetchContent 接入，
# 并链接 dart_cpp_bridge::runtime 目标（STATIC 库）。
#
# 为什么下游必须用 WHOLE_ARCHIVE 链接：
#   ffi_entry.cpp 里的 dcb_* 导出函数只被 Dart 经 FFI 调用，C++ 侧没有任何
#   引用。普通 STATIC 链接时，链接器不会把"未被引用"的 ffi_entry.obj 拉进
#   最终 DLL，导致 dcb_* 导出符号丢失、Dart FFI 查找失败。因此下游必须用
#   $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime> 链接，强制链入全部
#   对象。该 genex 需要 CMake 3.24+（OBJECT 库可免此步骤，但不可安装分发）。
#
# 依赖版本单点真相：
#   asio / async-simple 的版本只在本文件钉死。下游链接 dart_cpp_bridge::runtime
#   时会传递获得二者的 include 路径与编译定义，无需自行声明，不会版本漂移。
#
# 使用方式（monorepo 内下游项目）：
#   set(DCB_ROOT "<path-to>/dart/native")
#   add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)
#   add_library(my_bridge SHARED <your wire_dispatch.cpp> <your impl.cpp>)
#   target_link_libraries(my_bridge PRIVATE
#     $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
#
# 使用方式（外部项目，Phase 3 产品化后推荐）：
#   include(FetchContent)
#   FetchContent_Declare(
#     dart_cpp_bridge
#     GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
#     GIT_TAG        main  # 或钉死 tag/commit
#   )
#   FetchContent_MakeAvailable(dart_cpp_bridge)
#   target_link_libraries(my_bridge PRIVATE
#     $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dart_cpp_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# --- asio (header-only) ---
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

# --- async-simple (header-only) ---
# MakeAvailable 会执行其 CMakeLists，提供官方 async_simple_header_only
# INTERFACE 目标（include 目录 + Threads + cxx_std_20）。Windows 上 UTHREAD=OFF，
# async_simple 退化为纯 INTERFACE 库，零额外编译开销。
set(ASYNC_SIMPLE_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_BUILD_DEMO_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  async_simple
  GIT_REPOSITORY https://github.com/alibaba/async_simple.git
  GIT_TAG        1.4
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES  ""
)
FetchContent_MakeAvailable(async_simple)

# --- dart_api_dl (downloaded via fetch_dart_api.cmake) ---
set(DCB_DART_API_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dart_api)
if(NOT EXISTS "${DCB_DART_API_DIR}/dart_api_dl.c")
  message(FATAL_ERROR
    "Missing third_party/dart_api. Run: cmake -P ../fetch_dart_api.cmake")
endif()

# --- 基础运行时（STATIC 库，下游以 WHOLE_ARCHIVE 链接） ---
add_library(dcb_runtime STATIC
  src/runtime/runtime.cpp
  src/runtime/object_handle.cpp
  src/foreign_runtime.cpp
  src/ffi_entry.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
)

target_include_directories(dcb_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${DCB_DART_API_DIR}
)

target_compile_definitions(dcb_runtime PUBLIC DART_CPP_BRIDGE_BUILD)
target_link_libraries(dcb_runtime PUBLIC asio_iface async_simple_header_only)

if(WIN32)
  target_compile_definitions(dcb_runtime PUBLIC _WIN32_WINNT=0x0A00)
  target_compile_options(dcb_runtime PRIVATE /utf-8)
endif()

# Alias for downstream users
add_library(dart_cpp_bridge::runtime ALIAS dcb_runtime)
