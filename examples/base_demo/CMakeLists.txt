# =============================================================================
# base_demo — Hand-written wire dispatch example + C++ smoke test
# =============================================================================
#
# Build outputs:
#   - dart_cpp_bridge.dll : base runtime + hand-written demo_api wire dispatch
#   - dcb_smoke.exe       : pure C++ smoke test (no Dart VM)
#
# Dart tests (dart/test/) load the DLL built in this directory.
#
# The base runtime is brought in via add_subdirectory and linked with WHOLE_ARCHIVE
# for dart_cpp_bridge::runtime (STATIC library, forces all objects to be linked so dcb_* exports survive).
# asio / async-simple versions are pinned and propagated by the base library; no need to declare them here.
# =============================================================================

cmake_minimum_required(VERSION 3.25)
project(dcb_base_demo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# doctest's CMakeLists declares cmake_minimum_required(2.8.2); CMake >= 4.0
# rejects projects below 3.5 unless this escape hatch is set.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# --- Base runtime (asio / async-simple propagated from here) ---
set(DCB_PKG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../dart")
include("${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)

# --- doctest (header-only C++ test framework) ---
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(doctest)

# --- Shared / static library (Dart tests load this library) ---
# BUILD_SHARED_LIBS is passed in by the hook (ON=shared, OFF=static, default ON).
option(BUILD_SHARED_LIBS "Build shared library" ON)
# DCB_REGISTER_DISPATCH triggers demo_api.cpp to register dispatch at DLL load time.
add_library(dcb_base_demo demo_api.cpp)
target_compile_definitions(dcb_base_demo PRIVATE DCB_REGISTER_DISPATCH)
target_link_libraries(dcb_base_demo PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

# --- C++ smoke test (no Dart VM) ---
# Do not define DCB_REGISTER_DISPATCH: smoke calls dispatch functions directly, no registration needed,
# and does not rely on FFI entry points.
add_executable(dcb_smoke smoke_main.cpp demo_api.cpp)
target_link_libraries(dcb_smoke PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

# --- Channel throughput benchmark (no Dart VM) ---
add_executable(dcb_bench bench_channel.cpp)
target_link_libraries(dcb_bench PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

# --- Minimal repro for the bounded-mpsc race (temporary) ---
add_executable(dcb_repro repro_channel.cpp)
target_link_libraries(dcb_repro PRIVATE
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

# --- C++ unit tests (doctest) ---
add_executable(dcb_tests test_channel.cpp)
target_link_libraries(dcb_tests PRIVATE
  doctest
  $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

# --- CTest registration (smoke = runtime e2e, tests = doctest units, bench = perf) ---
enable_testing()
add_test(NAME dcb_smoke COMMAND dcb_smoke)
add_test(NAME dcb_tests COMMAND dcb_tests)
add_test(NAME dcb_bench COMMAND dcb_bench 4 n=10000)
set_tests_properties(dcb_smoke dcb_tests dcb_bench PROPERTIES TIMEOUT 180)

if(WIN32)
  if(MSVC)
    target_compile_options(dcb_base_demo PRIVATE /utf-8)
    target_compile_options(dcb_smoke PRIVATE /utf-8)
    target_compile_options(dcb_bench PRIVATE /utf-8)
    target_compile_options(dcb_tests PRIVATE /utf-8)
  else()
    target_compile_options(dcb_base_demo PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(dcb_smoke PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(dcb_bench PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    target_compile_options(dcb_tests PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
  endif()
endif()
