# =============================================================================
# dart_cpp_bridge codegen_demo — 可复制的项目模板
# =============================================================================
#
# 本 CMakeLists 演示如何构建一个使用 dart_cpp_bridge codegen 的动态库。
#
# 接入方式：
#   A) Monorepo（当前方式）：add_subdirectory 指向 dart/native，以
#      WHOLE_ARCHIVE 链接 dart_cpp_bridge::runtime（STATIC 库）。
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
#   - 修改源文件列表（native/api_impl/*.cpp）
#   - 修改 DCB_ROOT 或改用 FetchContent
# =============================================================================

cmake_minimum_required(VERSION 3.24)
project(dcb_codegen_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- 基础运行时（asio / async-simple 由此传递） ---
set(DCB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../dart/native")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- 生成物检查 ---
# 如果尚未跑 codegen，给出明确提示。
set(GEN_WIRE "${CMAKE_CURRENT_SOURCE_DIR}/native/generated/wire_dispatch.cpp")
if(NOT EXISTS "${GEN_WIRE}")
  message(FATAL_ERROR
    "Missing generated wire. Run codegen first:\n"
    "  cd codegen && dart run bin/codegen.dart scripts/run_codegen.py "
    "${CMAKE_CURRENT_SOURCE_DIR}/dart_cpp_bridge.yaml")
endif()

# --- 库 target ---
# 源文件 = 生成的 wire + 用户业务实现。基础运行时经链接传递进来。
# 生成的 wire 在 DLL 加载期无条件注册 dispatch，无需 DCB_REGISTER_DISPATCH。
# 复制本模板后：把 native/api_impl/*.cpp 改为你的实现文件。
# BUILD_SHARED_LIBS 由 hook 传入（ON=动态库，OFF=静态库，默认 ON）。
option(BUILD_SHARED_LIBS "Build shared library" ON)
add_library(dcb_codegen_demo
  native/generated/wire_dispatch.cpp
  native/api_impl/bridge_api.cpp
  native/api_impl/counter.cpp
)

# --- Include 路径 ---
# 复制本模板后：确保 native/generated 与你的 api 头目录在路径中。
# 基础库的 include 目录由 dart_cpp_bridge::runtime 传递，无需手写。
target_include_directories(dcb_codegen_demo
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/native
    ${CMAKE_CURRENT_SOURCE_DIR}/native/api
    ${CMAKE_CURRENT_SOURCE_DIR}/native/generated
)

target_link_libraries(dcb_codegen_demo PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

if(WIN32)
  target_compile_options(dcb_codegen_demo PRIVATE /utf-8)
endif()
