# =============================================================================
# dart_cpp_bridge codegen_demo — Reusable project template
# =============================================================================
#
# This CMakeLists demonstrates how to build a dynamic library using dart_cpp_bridge codegen.
# This file lives in the native/ directory; hook/build.dart points sourceDir: 'native' at it.
#
# Integration options:
#   A) Monorepo (current approach): automatically resolved via package_config.json
#      dart_cpp_bridge package path, linked with WHOLE_ARCHIVE to dart_cpp_bridge::runtime.
#   B) External project (FetchContent):
#
#      include(FetchContent)
#      FetchContent_Declare(
#        dart_cpp_bridge
#        GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
#        GIT_TAG        v2.2.0  # or pin a commit
#      )
#      FetchContent_MakeAvailable(dart_cpp_bridge)
#      target_link_libraries(my_bridge PRIVATE
#        $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
#
#   In both approaches, asio / stdexec / MPMCQueue versions are pinned and
#   propagated by the base library; no need to declare them yourself.
#   WHOLE_ARCHIVE forces all objects to be linked, keeping dcb_* FFI exports.
#
# Customization checklist:
#   - Change the target name (my_fancy_bridge → your library name)
#   - Change the source file list (api_impl/*.cpp)
# =============================================================================

cmake_minimum_required(VERSION 3.25)
project(my_fancy_bridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Find dart_cpp_bridge ---
# The Native Assets hook passes a decoded absolute DCB_PKG_PATH. Keep the
# package_config fallback for direct CMake invocation, but do not overwrite the
# URI-safe value supplied by the hook.
if(NOT DEFINED DCB_PKG_PATH OR DCB_PKG_PATH STREQUAL "")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../.dart_tool/package_config.json" _j)
  string(JSON _c LENGTH "${_j}" "packages")
  math(EXPR _e "${_c} - 1")
  foreach(_i RANGE ${_e})
    string(JSON _n GET "${_j}" "packages" ${_i} "name")
    if(_n STREQUAL "dart_cpp_bridge")
      string(JSON _u GET "${_j}" "packages" ${_i} "rootUri")
      break()
    endif()
  endforeach()
  if(NOT DEFINED _u)
    message(FATAL_ERROR "dart_cpp_bridge not in package_config.json")
  endif()
  if(_u MATCHES "^file://")
    if(WIN32)
      string(REGEX REPLACE "^file:///" "" DCB_PKG_PATH "${_u}")
    else()
      string(REGEX REPLACE "^file://" "" DCB_PKG_PATH "${_u}")
    endif()
  else()
    get_filename_component(DCB_PKG_PATH
      "${CMAKE_CURRENT_SOURCE_DIR}/../.dart_tool/${_u}" ABSOLUTE)
  endif()
endif()
include("${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- libuv (FetchContent) — required for the libuv worker tests ---
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

# --- Library target ---
# Sources = generated wire + user business implementation. Base runtime is propagated via linking.
# BUILD_SHARED_LIBS is passed in by the hook (ON=shared, OFF=static, default ON).
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library(my_fancy_bridge
  generated/wire_dispatch.cpp
  api_impl/bridge_api.cpp
  api_impl/counter.cpp
  api_impl/foreign_api.cpp
  api_impl/multi_runtime_api.cpp
)

# --- Include paths ---
target_include_directories(my_fancy_bridge
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
)

target_link_libraries(my_fancy_bridge PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime> uv_a)

if(WIN32)
  if(MSVC)
    target_compile_options(my_fancy_bridge PRIVATE /utf-8 /Zm2000)
  else()
    target_compile_options(my_fancy_bridge PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
  endif()
endif()
