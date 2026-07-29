# =============================================================================
# multi_runtime_demo — 多运行时非阻塞通信示例（codegen 自动生成绑定）
# =============================================================================
#
# 演示主调度器 (dcb::Runtime) 与独立 Worker 运行时之间通过
# co::mpsc / co::oneshot channel 进行非阻塞消息传递。
#
# 此文件位于 native/ 目录下，hook/build.dart 通过 sourceDir: 'native' 指定。
# 基础运行时经 dcb_find_package.cmake 接入，WHOLE_ARCHIVE 链接保住 dcb_* 导出。
# asio / async-simple 的版本由基础库钉死并传递，本文件无需声明依赖。
#
# 构建前需先运行 codegen：
#   dcb_gen generate dart_cpp_bridge.yaml
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dcb_multi_runtime_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 基础运行时（monorepo 直接路径） ---
set(DCB_PKG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../dart")
include("${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- 生成物检查 ---
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run: dcb_gen generate dart_cpp_bridge.yaml")
endif()

# --- 动态库 / 静态库 ---
# BUILD_SHARED_LIBS 由 hook 传入（ON=动态库，OFF=静态库，默认 ON）。
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
  target_compile_options(dcb_multi_runtime_demo PRIVATE /utf-8)
endif()
