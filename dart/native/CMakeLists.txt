# =============================================================================
# dart_cpp_bridge — 基础源码库
# =============================================================================
#
# 本目录是纯源码库，不构建独立 DLL。
# 下游项目（examples/base_demo, examples/codegen_demo）引用本目录的源文件
# 和头文件来构建各自的动态库。
#
# 使用方式（monorepo 内）：
#   set(DCB_ROOT "<path-to>/dart/native")
#   add_library(my_bridge SHARED
#     ${DCB_ROOT}/src/runtime/runtime.cpp
#     ${DCB_ROOT}/src/runtime/object_handle.cpp
#     ${DCB_ROOT}/src/ffi_entry.cpp
#     ${DCB_DART_API_DIR}/dart_api_dl.c
#     <your wire_dispatch.cpp>
#   )
#   target_include_directories(my_bridge PRIVATE ${DCB_ROOT}/include ${DCB_DART_API_DIR})
#
# 本 CMakeLists 用于独立构建时提供 FetchContent 依赖（asio / async-simple）。
# =============================================================================

cmake_minimum_required(VERSION 3.20)
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

# --- async-simple ---
set(ASYNC_SIMPLE_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ASYNC_SIMPLE_BUILD_DEMO_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  async_simple
  GIT_REPOSITORY https://github.com/alibaba/async_simple.git
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

# --- dart_api_dl (downloaded via fetch_dart_api.cmake) ---
set(DCB_DART_API_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dart_api)
if(NOT EXISTS "${DCB_DART_API_DIR}/dart_api_dl.c")
  message(FATAL_ERROR
    "Missing third_party/dart_api. Run: cmake -P ../fetch_dart_api.cmake")
endif()

# --- 基础源码库（STATIC，供下游链接） ---
add_library(dcb_runtime STATIC
  src/runtime/runtime.cpp
  src/runtime/object_handle.cpp
  src/ffi_entry.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
)

target_include_directories(dcb_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${DCB_DART_API_DIR}
)

target_compile_definitions(dcb_runtime PUBLIC DART_CPP_BRIDGE_BUILD)
target_link_libraries(dcb_runtime PUBLIC asio_iface async_simple_iface)

if(WIN32)
  target_compile_definitions(dcb_runtime PUBLIC _WIN32_WINNT=0x0A00)
  target_compile_options(dcb_runtime PRIVATE /utf-8)
endif()

# Alias for downstream FetchContent users
add_library(dart_cpp_bridge::runtime ALIAS dcb_runtime)
