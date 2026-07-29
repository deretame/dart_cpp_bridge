# =============================================================================
# dart_cpp_bridge — Base runtime library
# =============================================================================
#
# This directory contains the base runtime library. Downstream projects
# integrate via add_subdirectory / FetchContent and link against the
# dart_cpp_bridge::runtime target (STATIC library).
#
# Why downstream MUST link with WHOLE_ARCHIVE:
#   The dcb_* exported functions in ffi_entry.cpp are only called from Dart
#   via FFI — there are no C++ references to them. With normal STATIC linking,
#   the linker will not pull in the "unreferenced" ffi_entry.obj into the final
#   DLL, causing dcb_* symbols to be missing and Dart FFI lookups to fail.
#   Therefore downstream must link with
#   $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime> to force all objects
#   to be included. This genex requires CMake 3.24+ (OBJECT libraries avoid
#   this step but cannot be installed/distributed).
#
# Single source of truth for dependency versions:
#   asio / async-simple versions are pinned only in this file. Downstream
#   targets linking dart_cpp_bridge::runtime transitively receive both include
#   paths and compile definitions — no need to declare them separately, no
#   version drift.
#
# Usage (monorepo downstream project):
#   set(DCB_ROOT "<path-to>/dart/native")
#   add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)
#   add_library(my_bridge SHARED <your wire_dispatch.cpp> <your impl.cpp>)
#   target_link_libraries(my_bridge PRIVATE
#     $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
#
# Usage (external project, recommended after Phase 3 productization):
#   include(FetchContent)
#   FetchContent_Declare(
#     dart_cpp_bridge
#     GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
#     GIT_TAG        main  # or pin a tag/commit
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
# MakeAvailable runs its CMakeLists, providing the official
# async_simple_header_only INTERFACE target (include dirs + Threads +
# cxx_std_20). On Windows with UTHREAD=OFF, async_simple degrades to a
# pure INTERFACE library with zero extra compilation overhead.
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

# --- moodycamel::ConcurrentQueue (header-only, lock-free MPMC queue) ---
FetchContent_Declare(
  concurrentqueue
  GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
  GIT_TAG        v1.0.4
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(concurrentqueue)

# --- dart_api_dl (downloaded via fetch_dart_api.cmake) ---
set(DCB_DART_API_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dart_api)
if(NOT EXISTS "${DCB_DART_API_DIR}/dart_api_dl.c")
  message(FATAL_ERROR
    "Missing third_party/dart_api. Run: cmake -P cmake/fetch_dart_api.cmake")
endif()

# --- Base runtime (STATIC library, downstream links with WHOLE_ARCHIVE) ---
add_library(dcb_runtime STATIC
  src/runtime/runtime.cpp
  src/runtime/object_handle.cpp
  src/foreign_runtime.cpp
  src/cbridge.cpp
  src/dcb_codec.c
  src/ffi_entry.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
)
set_target_properties(dcb_runtime PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(dcb_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${DCB_DART_API_DIR}
)

target_compile_definitions(dcb_runtime PUBLIC DART_CPP_BRIDGE_BUILD)
target_link_libraries(dcb_runtime PUBLIC asio_iface async_simple_header_only concurrentqueue)

if(WIN32)
  target_compile_definitions(dcb_runtime PUBLIC _WIN32_WINNT=0x0A00)
  target_compile_options(dcb_runtime PRIVATE /utf-8)
endif()

# Alias for downstream users
add_library(dart_cpp_bridge::runtime ALIAS dcb_runtime)

# --- Optional: build the runtime directly as a SHARED library ---
# Used by the Native Assets build hook (dart/hook/build.dart) to produce a
# self-contained dart_cpp_bridge.{dll,so,dylib} whose dcb_* symbols are exported
# (DCB_API -> dllexport because DART_CPP_BRIDGE_BUILD is defined) and loadable
# via @Native. Downstream add_subdirectory users keep the STATIC + WHOLE_ARCHIVE
# flow; this stays OFF by default so it is not triggered for them.
option(DCB_BUILD_SHARED "Build dart_cpp_bridge runtime as a shared library" OFF)
if(DCB_BUILD_SHARED)
  add_library(dcb_shared SHARED
    src/runtime/runtime.cpp
    src/runtime/object_handle.cpp
    src/foreign_runtime.cpp
    src/cbridge.cpp
    src/dcb_codec.c
    src/ffi_entry.cpp
    ${DCB_DART_API_DIR}/dart_api_dl.c
  )
  target_include_directories(dcb_shared
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${DCB_DART_API_DIR}
  )
  target_compile_definitions(dcb_shared PRIVATE DART_CPP_BRIDGE_BUILD)
  target_link_libraries(dcb_shared PRIVATE asio_iface async_simple_header_only concurrentqueue)
  if(WIN32)
    target_compile_definitions(dcb_shared PRIVATE _WIN32_WINNT=0x0A00)
    target_compile_options(dcb_shared PRIVATE /utf-8)
    # Use static CRT (/MT) to avoid depending on the system MSVCP140.dll.
    # The system copy (14.00, VS 2015) is incompatible with MSVC 19.51 code
    # when called via Dart @Native — std::mutex::lock() crashes in mtx_do_lock.
    set_property(TARGET dcb_shared PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()
  set_target_properties(dcb_shared PROPERTIES OUTPUT_NAME dart_cpp_bridge)
endif()
