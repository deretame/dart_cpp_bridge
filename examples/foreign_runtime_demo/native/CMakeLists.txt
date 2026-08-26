# =============================================================================
# foreign_runtime_demo — Foreign runtime integration example (libuv + UvScheduler)
# =============================================================================
#
# Demonstrates a non-asio event loop (libuv) integrating with the bridge's
# stdexec sender world: UvWorker exposes a UvScheduler (uv_scheduler.hpp) and
# business code composes it with starts_on / schedule_after / uv_work.
#
# This file lives in the native/ directory; hook/build.dart points sourceDir: 'native' at it.
# The base runtime is brought in via dcb_find_package.cmake and linked with WHOLE_ARCHIVE to keep dcb_* exports.
# asio / stdexec / MPMCQueue versions are pinned and propagated by the base
# library; only libuv is declared here.
#
# Run codegen before building:
#   dcb_gen_tool generate dart_cpp_bridge.yaml
# =============================================================================

cmake_minimum_required(VERSION 3.25)
project(dcb_foreign_runtime_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Base runtime (direct monorepo path) ---
set(DCB_PKG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../dart")
include("${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
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

# --- Generated artifacts check ---
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run: dcb_gen_tool generate dart_cpp_bridge.yaml")
endif()

# --- Shared / static library ---
# BUILD_SHARED_LIBS is passed in by the hook (ON=shared, OFF=static, default ON).
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library(dcb_foreign_runtime_demo
  generated/wire_dispatch.cpp
  api_impl/foreign_api.cpp
)

target_include_directories(dcb_foreign_runtime_demo
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
)

target_link_libraries(dcb_foreign_runtime_demo PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime> uv_a)

if(WIN32)
  if(MSVC)
    target_compile_options(dcb_foreign_runtime_demo PRIVATE /utf-8)
  else()
    target_compile_options(dcb_foreign_runtime_demo PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
  endif()
endif()
