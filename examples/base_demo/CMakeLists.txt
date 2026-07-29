# =============================================================================
# base_demo — 手写 wire dispatch 示例 + C++ smoke test
# =============================================================================
#
# 构建产物：
#   - dart_cpp_bridge.dll : 包含基础运行时 + 手写 demo_api wire dispatch
#   - dcb_smoke.exe       : 纯 C++ smoke test（无 Dart VM）
#
# Dart 测试（dart/test/）加载本目录构建的 DLL。
#
# 基础运行时经 add_subdirectory 接入，并以 WHOLE_ARCHIVE 链接
# dart_cpp_bridge::runtime（STATIC 库，强制链入全部对象以保住 dcb_* 导出）。
# asio / async-simple 的版本由基础库钉死并传递，本文件无需声明依赖。
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dcb_base_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 基础运行时（asio / async-simple 由此传递） ---
set(DCB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../dart/native")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- 动态库 / 静态库（Dart 测试加载此库） ---
# BUILD_SHARED_LIBS 由 hook 传入（ON=动态库，OFF=静态库，默认 ON）。
option(BUILD_SHARED_LIBS "Build shared library" ON)
# DCB_REGISTER_DISPATCH 触发 demo_api.cpp 在 DLL 加载期注册 dispatch。
add_library(dcb_base_demo demo_api.cpp)
target_compile_definitions(dcb_base_demo PRIVATE DCB_REGISTER_DISPATCH)
target_link_libraries(dcb_base_demo PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

# --- C++ smoke test（无 Dart VM） ---
# 不定义 DCB_REGISTER_DISPATCH：smoke 直接调用 dispatch 函数，无需注册，
# 也不依赖 FFI 入口。
add_executable(dcb_smoke smoke_main.cpp demo_api.cpp)
target_link_libraries(dcb_smoke PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

if(WIN32)
  target_compile_options(dcb_base_demo PRIVATE /utf-8)
  target_compile_options(dcb_smoke PRIVATE /utf-8)
endif()
