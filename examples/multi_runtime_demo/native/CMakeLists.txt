# =============================================================================
# multi_runtime_demo — Multi-runtime non-blocking communication example (codegen auto-generates bindings)
# =============================================================================
#
# Demonstrates non-blocking messaging between the main dispatcher (dcb::Runtime)
# and independent Worker runtimes through co::mpsc / co::oneshot channels.
#
# This file lives in the native/ directory; hook/build.dart points sourceDir: 'native' at it.
# The base runtime is brought in via dcb_find_package.cmake and linked with WHOLE_ARCHIVE to keep dcb_* exports.
# asio / async-simple versions are pinned and propagated by the base library; no need to declare them here.
#
# Run codegen before building:
#   dcb_gen_tool generate dart_cpp_bridge.yaml
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dcb_multi_runtime_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Base runtime (direct monorepo path) ---
set(DCB_PKG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../dart")
include("${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- Generated artifacts check ---
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run: dcb_gen_tool generate dart_cpp_bridge.yaml")
endif()

# --- Shared / static library ---
# BUILD_SHARED_LIBS is passed in by the hook (ON=shared, OFF=static, default ON).
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library(dcb_multi_runtime_demo
  generated/wire_dispatch.cpp
  api_impl/multi_runtime_api.cpp
)

target_include_directories(dcb_multi_runtime_demo
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
)

target_link_libraries(dcb_multi_runtime_demo PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

if(WIN32)
  if(MSVC)
    target_compile_options(dcb_multi_runtime_demo PRIVATE /utf-8)
  else()
    target_compile_options(dcb_multi_runtime_demo PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
  endif()
endif()
