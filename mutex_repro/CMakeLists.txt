cmake_minimum_required(VERSION 3.24)
project(mutex_repro LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(mutex_repro SHARED native/mutex_api.cpp)

# 关键：使用 /MD 动态 CRT（默认值），让 DLL 依赖系统 MSVCP140.dll。
# 如果要对比 /MT 静态 CRT 的效果，取消下面的注释：
# set_property(TARGET mutex_repro PROPERTY
#   MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(mutex_repro PROPERTIES
  OUTPUT_NAME mutex_repro
)
