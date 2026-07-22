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
# Header-oriented use: only need includes + optional static lib.
FetchContent_GetProperties(async_simple)
if(NOT async_simple_POPULATED)
  FetchContent_Populate(async_simple)
endif()

add_library(async_simple_iface INTERFACE)
target_include_directories(async_simple_iface INTERFACE ${async_simple_SOURCE_DIR})
# async_simple coro is mostly header-only when not using uthread; link nothing.

# --- dart_api_dl (vendored under third_party) ---
set(DCB_DART_API_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dart_api)
if(NOT EXISTS "${DCB_DART_API_DIR}/dart_api_dl.c")
  message(FATAL_ERROR
    "Missing third_party/dart_api. Run: cmake -P cmake/fetch_dart_api.cmake")
endif()

add_library(dart_cpp_bridge SHARED
  src/runtime/runtime.cpp
  src/wire/demo_api.cpp
  src/ffi_entry.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
)

target_include_directories(dart_cpp_bridge
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${DCB_DART_API_DIR}
)

target_compile_definitions(dart_cpp_bridge PRIVATE DART_CPP_BRIDGE_BUILD)
target_link_libraries(dart_cpp_bridge PUBLIC asio_iface async_simple_iface)

if(WIN32)
  target_compile_definitions(dart_cpp_bridge PRIVATE _WIN32_WINNT=0x0A00)
  target_compile_options(dart_cpp_bridge PRIVATE /utf-8)
endif()

# Export names for downstream FetchContent users
add_library(dart_cpp_bridge::runtime ALIAS dart_cpp_bridge)

# Optional: C++ smoke test (no Dart) — links core sources statically (no DLL export).
option(DCB_BUILD_SMOKE "Build native smoke test" ON)
if(DCB_BUILD_SMOKE)
  add_executable(dcb_smoke
    examples/phase1_demo/smoke_main.cpp
    src/runtime/runtime.cpp
    src/wire/demo_api.cpp
  )
  target_include_directories(dcb_smoke PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${DCB_DART_API_DIR}
  )
  target_link_libraries(dcb_smoke PRIVATE asio_iface async_simple_iface)
  if(WIN32)
    target_compile_definitions(dcb_smoke PRIVATE _WIN32_WINNT=0x0A00)
    target_compile_options(dcb_smoke PRIVATE /utf-8)
  endif()
endif()
