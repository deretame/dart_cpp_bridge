# =============================================================================
# dart_cpp_bridge codegen_demo — 可复制的项目模板
# =============================================================================
#
# 本 CMakeLists 演示如何构建一个使用 dart_cpp_bridge codegen 的动态库。
# 此文件位于 native/ 目录下，hook/build.dart 通过 sourceDir: 'native' 指定。
#
# 接入方式：
#   A) Monorepo（当前方式）：通过 package_config.json 自动解析
#      dart_cpp_bridge 包路径，以 WHOLE_ARCHIVE 链接 dart_cpp_bridge::runtime。
#   B) 外部项目（FetchContent，Phase 3 产品化后推荐）：
#
#      include(FetchContent)
#      FetchContent_Declare(
#        dart_cpp_bridge
#        GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
#        GIT_TAG        main  # 或钉死 tag/commit
#      )
#      FetchContent_MakeAvailable(dart_cpp_bridge)
#      target_link_libraries(my_bridge PRIVATE
#        $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
#
#   两种方式下 asio / async-simple 的版本都由基础库钉死并传递，无需自行声明。
#   WHOLE_ARCHIVE 强制链入全部对象，保住 dcb_* FFI 导出（需 CMake 3.24+）。
#
# 自定义清单：
#   - 修改 target 名（dcb_codegen_demo → 你的库名）
#   - 修改源文件列表（api_impl/*.cpp）
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dcb_codegen_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Find dart_cpp_bridge (reads .dart_tool/package_config.json) ---
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
if(_u MATCHES "^file:///")
  string(REGEX REPLACE "^file:///" "" DCB_PKG_PATH "${_u}")
elseif(_u MATCHES "^file://")
  string(REGEX REPLACE "^file://" "" DCB_PKG_PATH "${_u}")
else()
  get_filename_component(DCB_PKG_PATH
    "${CMAKE_CURRENT_SOURCE_DIR}/../.dart_tool/${_u}" ABSOLUTE)
endif()
include("${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- libuv (FetchContent) — foreign_executor 测试需要 ---
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
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run: dcb_gen generate dart_cpp_bridge.yaml")
endif()

# --- 库 target ---
# 源文件 = 生成的 wire + 用户业务实现。基础运行时经链接传递进来。
# BUILD_SHARED_LIBS 由 hook 传入（ON=动态库，OFF=静态库，默认 ON）。
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library(dcb_codegen_demo
  generated/wire_dispatch.cpp
  api_impl/bridge_api.cpp
  api_impl/counter.cpp
  api_impl/foreign_api.cpp
  api_impl/multi_runtime_api.cpp
)

# --- Include 路径 ---
target_include_directories(dcb_codegen_demo
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/api
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
)

target_link_libraries(dcb_codegen_demo PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime> uv_a)

if(WIN32)
  target_compile_options(dcb_codegen_demo PRIVATE /utf-8)
endif()
