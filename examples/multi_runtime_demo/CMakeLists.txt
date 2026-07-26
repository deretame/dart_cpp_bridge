# =============================================================================
# multi_runtime_demo — 多运行时非阻塞通信示例（codegen 自动生成绑定）
# =============================================================================
#
# 演示主调度器 (dcb::Runtime) 与独立 Worker 运行时之间通过
# co::mpsc / co::oneshot channel 进行非阻塞消息传递。
#
# 构建产物：
#   - dart_cpp_bridge.dll : 基础运行时 + codegen 生成的 wire dispatch
#
# Dart 测试加载本目录构建的 DLL。
#
# 基础运行时经 add_subdirectory 接入，并以 WHOLE_ARCHIVE 链接
# dart_cpp_bridge::runtime（STATIC 库，强制链入全部对象以保住 dcb_* 导出）。
# asio / async-simple 的版本由基础库钉死并传递，本文件无需声明依赖。
#
# 构建前需先运行 codegen：
#   cd ../../codegen
#   dart run bin/codegen.dart scripts/run_codegen.py ../examples/multi_runtime_demo/dart_cpp_bridge.yaml
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dcb_multi_runtime_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 基础运行时（asio / async-simple 由此传递） ---
set(DCB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../dart/native")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- 生成物检查 ---
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/native/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run codegen first:\n"
    "  cd ../../codegen && dart run bin/codegen.dart scripts/run_codegen.py "
    "../examples/multi_runtime_demo/dart_cpp_bridge.yaml")
endif()

# --- 动态库 ---
# 生成的 wire 在 DLL 加载期无条件注册 dispatch，无需 DCB_REGISTER_DISPATCH。
add_library(dart_cpp_bridge SHARED
  native/generated/wire_dispatch.cpp
  native/api_impl/multi_runtime_api.cpp
)

target_include_directories(dart_cpp_bridge
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/native
    ${CMAKE_CURRENT_SOURCE_DIR}/native/api
    ${CMAKE_CURRENT_SOURCE_DIR}/native/generated
)

target_link_libraries(dart_cpp_bridge PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

if(WIN32)
  target_compile_options(dart_cpp_bridge PRIVATE /utf-8)
endif()
