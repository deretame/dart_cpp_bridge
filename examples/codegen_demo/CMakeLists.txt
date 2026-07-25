# =============================================================================
# dart_cpp_bridge codegen_demo — 可复制的项目模板
# =============================================================================
#
# 本 CMakeLists 演示如何构建一个使用 dart_cpp_bridge codegen 的动态库。
#
# 接入方式：
#   A) Monorepo（当前方式）：设置 DCB_ROOT 指向 dart_cpp_bridge 仓库根，
#      复用已下载的 asio/async-simple（build/_deps）。
#   B) 外部项目（FetchContent，Phase 3 产品化后推荐）：
#
#      include(FetchContent)
#      FetchContent_Declare(
#        dart_cpp_bridge
#        GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
#        GIT_TAG        main  # 或钉死 tag/commit
#      )
#      FetchContent_MakeAvailable(dart_cpp_bridge)
#      target_link_libraries(my_bridge PRIVATE dart_cpp_bridge)
#      # dart_cpp_bridge 目标已 PUBLIC 暴露 asio + async-simple
#
# 自定义清单：
#   - 修改 target 名（dcb_codegen_demo → 你的库名）
#   - 修改源文件列表（native/api_impl/*.cpp）
#   - 修改 DCB_ROOT 或改用 FetchContent
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(dcb_codegen_demo LANGUAGES C CXX)

# --- 依赖解析 ---
# 优先复用 monorepo 主工程已下载的 asio/async-simple（避免二次 git clone）。
# 外部项目可设置 -DDCB_DEPS_DIR=... 或改用 FetchContent（见上方说明）。

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# --- 生成物检查 ---
# 如果尚未跑 codegen，给出明确提示。
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/native/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run codegen first:\n"
    "  cd codegen && dart run bin/codegen.dart scripts/run_codegen.py "
    "${CMAKE_CURRENT_SOURCE_DIR}/dart_cpp_bridge.yaml")
endif()

# --- 动态库 target ---
# 源文件 = 桥运行时 + FFI 入口 + 生成的 wire + 用户业务实现。
# 复制本模板后：把 native/api_impl/*.cpp 改为你的实现文件。
add_library(dcb_codegen_demo SHARED
  ${DCB_ROOT}/src/runtime/runtime.cpp
  ${DCB_ROOT}/src/runtime/object_handle.cpp
  ${DCB_ROOT}/src/ffi_entry.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
  native/generated/wire_dispatch.cpp
  native/api_impl/bridge_api.cpp
  native/api_impl/counter.cpp
)

# --- Include 路径 ---
# 复制本模板后：确保 DCB_ROOT/include 和 native/generated 在路径中。
target_include_directories(dcb_codegen_demo
  PRIVATE
    ${DCB_ROOT}/include
    ${DCB_DART_API_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/native
    ${CMAKE_CURRENT_SOURCE_DIR}/native/api
    ${CMAKE_CURRENT_SOURCE_DIR}/native/generated
)

# --- 编译选项 ---
target_compile_definitions(dcb_codegen_demo PRIVATE DART_CPP_BRIDGE_BUILD)
target_link_libraries(dcb_codegen_demo PUBLIC asio_iface async_simple_iface)

if(WIN32)
  target_compile_definitions(dcb_codegen_demo PRIVATE _WIN32_WINNT=0x0A00)
  target_compile_options(dcb_codegen_demo PRIVATE /utf-8)
endif()
