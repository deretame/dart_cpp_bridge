# =============================================================================
# dart_cpp_bridge — Base runtime library
# =============================================================================
#
# This directory contains the base runtime library. Downstream projects
# integrate via add_subdirectory / FetchContent and link against the
# dart_cpp_bridge::runtime target (STATIC library).
#
# Why downstream MUST link with WHOLE_ARCHIVE:
#   The dcb_* exported functions in ffi_entry.cpp are only called from Dart
#   via FFI — there are no C++ references to them. With normal STATIC linking,
#   the linker will not pull in the "unreferenced" ffi_entry.obj into the final
#   DLL, causing dcb_* symbols to be missing and Dart FFI lookups to fail.
#   Therefore downstream must link with
#   $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime> to force all objects
#   to be included. This genex requires CMake 3.25+ (OBJECT libraries avoid
#   this step but cannot be installed/distributed).
#
# Single source of truth for dependency versions:
#   asio / stdexec / MPMCQueue versions are pinned only in this file.
#   Downstream targets linking dart_cpp_bridge::runtime transitively receive
#   both include paths and compile definitions — no need to declare them
#   separately, no version drift.
#
# Usage (monorepo downstream project):
#   set(DCB_ROOT "<path-to>/dart/native")
#   add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)
#   add_library(my_bridge SHARED <your wire_dispatch.cpp> <your impl.cpp>)
#   target_link_libraries(my_bridge PRIVATE
#     $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
#
# Usage (external project):
#   include(FetchContent)
#   FetchContent_Declare(
#     dart_cpp_bridge
#     GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
#     GIT_TAG        v2.2.0  # or pin a commit
#   )
#   FetchContent_MakeAvailable(dart_cpp_bridge)
#   target_link_libraries(my_bridge PRIVATE
#     $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)
# =============================================================================

cmake_minimum_required(VERSION 3.25)
project(dart_cpp_bridge LANGUAGES C CXX)

include(FetchContent)

# =============================================================================
# Optional dependency fetching (decided by the parent project)
#   DCB_FETCH_STDEXEC   default ON  - this library FetchContent-fetches and
#                       patches stdexec. Set OFF when the parent provides the
#                       STDEXEC::stdexec target itself (vcpkg, FetchContent
#                       with a pinned version, ...); this library will not
#                       fetch or patch in that case.
#   DCB_FETCH_ASIO      default ON  - this library fetches standalone asio
#                       (chriskohlhoff). Set OFF when the parent provides
#                       asio (an `asio_iface` target or find_package(asio)).
#   DCB_USE_BOOST_ASIO  default OFF - use boost::asio instead of standalone
#                       asio. When enabled, standalone asio is not fetched
#                       (DCB_FETCH_ASIO is ignored); Boost is provided by the
#                       parent (vcpkg / BOOST_ROOT / system). The business
#                       namespace switches to boost::asio via the DCB_ASIO_NS
#                       macro.
# =============================================================================
option(DCB_FETCH_STDEXEC "Fetch stdexec via FetchContent; OFF when the parent provides STDEXEC::stdexec" ON)
option(DCB_FETCH_ASIO "Fetch standalone asio via FetchContent; OFF when the parent provides asio" ON)
option(DCB_FETCH_MPMCQUEUE "Fetch rigtorp::MPMCQueue via FetchContent; OFF when the parent provides MPMCQueue" ON)
option(DCB_FETCH_DART_API "Fetch Dart API DL headers automatically when they are missing" ON)
option(DCB_USE_BOOST_ASIO "Use boost::asio instead of standalone asio" OFF)
set(DCB_DART_API_VERSION "3.10.0" CACHE STRING "Dart SDK version used for dart_api_dl headers")
set(DCB_DART_API_DIR "" CACHE PATH
  "Directory containing dart_api_dl.c and the Dart API DL headers")

# --- asio (header-only) ---
# Version bumped to 1.31.0: stdexec's official standalone-asio integration
# (STDEXEC_ENABLE_ASIO) pins asio-1-31-0 as its floor.
if(DCB_USE_BOOST_ASIO)
  # Boost.Asio mode: standalone asio is not fetched. Boost is provided by
  # stdexec's ConfigureASIO (rapids_cpm_find Boost) or by the parent; the
  # Boost header paths are resolved from the Boost::asio target after stdexec
  # has been populated (see below). Here we only create asio_iface and declare
  # the macro.
  if(NOT TARGET asio_iface)
    add_library(asio_iface INTERFACE)
    target_compile_definitions(asio_iface INTERFACE DCB_USE_BOOST_ASIO)
  endif()
elseif(TARGET asio_iface)
  # The parent already owns the bridge-facing interface. Do not declare a
  # second FetchContent dependency just because DCB_FETCH_ASIO defaults ON.
elseif(TARGET asio OR TARGET asio::asio)
  # Reuse a conventional parent-provided target and normalize it to the
  # interface target consumed by the bridge.
  add_library(asio_iface INTERFACE)
  if(TARGET asio)
    target_link_libraries(asio_iface INTERFACE asio)
  else()
    target_link_libraries(asio_iface INTERFACE asio::asio)
  endif()
elseif(DCB_FETCH_ASIO)
  FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-31-0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(asio)

  if(NOT TARGET asio_iface)
    add_library(asio_iface INTERFACE)
    target_include_directories(asio_iface INTERFACE ${asio_SOURCE_DIR}/asio/include)
    target_compile_definitions(asio_iface INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
  endif()

  # The asio repo has no CMakeLists.txt, so MakeAvailable only populates the
  # source tree — no target is created. stdexec's ConfigureASIO reuses an
  # existing target named `asio` (import_standalone_asio returns early when it
  # finds one) instead of CPM-fetching its own asio-1-31-0. Create that target
  # as an alias of asio_iface so the whole build shares one asio copy/version.
  if(NOT TARGET asio)
    add_library(asio ALIAS asio_iface)
  endif()
else()
  # Parent-provided asio: reuse an existing `asio_iface` target if present,
  # otherwise find_package(asio).
  find_package(asio CONFIG QUIET)
  if(TARGET asio::asio)
    add_library(asio_iface INTERFACE)
    target_link_libraries(asio_iface INTERFACE asio::asio)
  else()
    message(FATAL_ERROR
      "DCB_FETCH_ASIO=OFF but no asio found: provide an `asio_iface` target "
      "or a CMake package `asio` (e.g. vcpkg), or keep DCB_FETCH_ASIO=ON.")
  endif()
  # stdexec's standalone-Asio integration looks for the conventional `asio`
  # target before deciding whether it must fetch its own copy. Preserve the
  # parent-provided interface under that name in OFF mode as well; otherwise
  # disabling DCB_FETCH_ASIO can still trigger a second CPM/FetchContent
  # download during stdexec configuration.
  if(NOT TARGET asio)
    add_library(asio ALIAS asio_iface)
  endif()
endif()

# Normalize every standalone-Asio path to the target name expected by
# stdexec's ConfigureASIO. This is intentionally done after all branches:
# parent projects may provide `asio_iface`, `asio`, or `asio::asio`.
if(NOT TARGET asio)
  add_library(asio ALIAS asio_iface)
endif()

# MinGW-style toolchains (e.g. MSYS2 clang/gcc) do not link the Winsock
# libraries by default the way MSVC does; asio needs them on Windows.
if(WIN32 AND NOT MSVC AND TARGET asio_iface)
  target_link_libraries(asio_iface INTERFACE ws2_32 mswsock)
endif()

# --- rigtorp::MPMCQueue (header-only, lock-free bounded MPMC queue) ---
# Strict-FIFO bounded queue (ring buffer, atomic ticket order) used as the
# buffer of co::mpsc::bounded channels. Pinned to commit b9808ede (master,
# 2024-05). MIT license.
if(NOT TARGET MPMCQueue AND NOT TARGET MPMCQueue::MPMCQueue)
  if(DCB_FETCH_MPMCQUEUE)
    FetchContent_Declare(
      mpmcqueue
      GIT_REPOSITORY https://github.com/rigtorp/MPMCQueue.git
      GIT_TAG        b9808ede08f26fa9df4df4e081d19cace8f6c6ea
      GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(mpmcqueue)
  else()
    find_package(MPMCQueue CONFIG QUIET)
  endif()
endif()

if(NOT TARGET MPMCQueue)
  if(TARGET MPMCQueue::MPMCQueue)
    add_library(MPMCQueue ALIAS MPMCQueue::MPMCQueue)
  else()
    message(FATAL_ERROR
      "No MPMCQueue target found. Provide MPMCQueue or MPMCQueue::MPMCQueue, "
      "or keep DCB_FETCH_MPMCQUEUE=ON.")
  endif()
endif()

# --- stdexec (header-only, std::execution P2300 reference implementation) ---
# Migration experiment (branch feat/stdexec-migration): async-simple is fully
# replaced by stdexec senders/receivers/schedulers. Provides the
# `STDEXEC::stdexec` INTERFACE target (include dirs + generated
# stdexec_version_config.hpp).
# Pinned to the nvhpc-26.05 release tag (lightweight tag → commit
# 6d7ad689f4d4, 2026-05-11, project VERSION 0.11.0; main has no formal
# release tags).
# This tag's own CMakeLists requires CMake >= 3.25 (main requires 3.28),
# hence cmake_minimum_required above. When fetched below, tests/docs default
# OFF; examples default ON, so disable them explicitly.
#
# PATCH_COMMAND: stdexec's __any_allocator relies on [class.access] (members
# of a class template may access private members of other specializations),
# which MSVC does not implement; the patch adds an explicit friend
# declaration. stdexec::task's type-erased task_scheduler is built on
# __any_allocator, so without the patch no MSVC build using stdexec::task
# (or a scheduler that cannot be stored inline) would compile.
if(TARGET STDEXEC::stdexec OR TARGET STDEXEC::asioexec)
  if(NOT TARGET STDEXEC::stdexec OR NOT TARGET STDEXEC::asioexec)
    message(FATAL_ERROR
      "The parent provided an incomplete stdexec setup: both "
      "STDEXEC::stdexec and STDEXEC::asioexec are required.")
  endif()
elseif(DCB_FETCH_STDEXEC)
  # Enable the Asio adapter (experimental::execution::asio): use_sender
  # completion token, asio_thread_pool (STDEXEC::asio_pool alias), etc. It is
  # header-only and links against the `asio` alias target declared above.
  # The asio implementation follows DCB_USE_BOOST_ASIO: boost / standalone.
  # Keep these settings local to the dependency we own; parent-provided
  # stdexec must retain its own configuration.
  set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(STDEXEC_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(STDEXEC_INSTALL OFF CACHE BOOL "" FORCE)
  set(STDEXEC_ENABLE_ASIO ON CACHE BOOL "" FORCE)
  if(DCB_USE_BOOST_ASIO)
    set(STDEXEC_ASIO_IMPLEMENTATION "boost" CACHE STRING "" FORCE)
  else()
    set(STDEXEC_ASIO_IMPLEMENTATION "standalone" CACHE STRING "" FORCE)
  endif()
  FetchContent_Declare(
    stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG        nvhpc-26.05
    GIT_SHALLOW    TRUE
    PATCH_COMMAND  "${CMAKE_COMMAND}"
                     -DSTDEXEC_SRC=<SOURCE_DIR>
                     -P "${CMAKE_CURRENT_LIST_DIR}/cmake/patch_stdexec_any_allocator.cmake"
  )
  FetchContent_MakeAvailable(stdexec)
  # Boost.Asio mode: stdexec's ConfigureASIO has provided Boost::asio via
  # rapids_cpm_find (official boost-cmake, an INTERFACE target). Grab the
  # Boost header paths from it for the business side (DCB_ASIO_NS =
  # boost::asio); the same applies when the parent provides Boost::asio in
  # another way (e.g. a vcpkg config).
  if(DCB_USE_BOOST_ASIO AND NOT Boost_INCLUDE_DIRS)
    if(TARGET Boost::asio)
      get_target_property(_dcb_boost_incs Boost::asio INTERFACE_INCLUDE_DIRECTORIES)
      if(_dcb_boost_incs)
        target_include_directories(asio_iface INTERFACE ${_dcb_boost_incs})
      endif()
    else()
      find_package(Boost REQUIRED)
      target_include_directories(asio_iface INTERFACE ${Boost_INCLUDE_DIRS})
    endif()
  endif()
else()
  # Parent-provided stdexec (vcpkg, FetchContent with a pinned version, ...):
  # require the STDEXEC::stdexec target to exist. Note: in this mode the
  # patch_stdexec_any_allocator.cmake patch is NOT applied, so the parent must
  # use a stdexec version that already contains the __any_allocator fix (or
  # handle it itself); the STDEXEC_* options must also be configured by the
  # parent before add_subdirectory (setting them after MakeAvailable has no
  # effect).
  find_package(stdexec CONFIG QUIET)
  if(NOT TARGET STDEXEC::stdexec OR NOT TARGET STDEXEC::asioexec)
    message(FATAL_ERROR
      "DCB_FETCH_STDEXEC=OFF but no complete stdexec setup was found. "
      "The parent project must provide STDEXEC::stdexec and "
      "STDEXEC::asioexec (vcpkg or FetchContent with STDEXEC_ENABLE_ASIO "
      "matching DCB_USE_BOOST_ASIO).")
  endif()
endif()

# --- dart_api_dl (downloaded automatically for FetchContent consumers) ---
set(_dcb_dart_api_source_dir "${CMAKE_CURRENT_LIST_DIR}/third_party/dart_api")
set(_dcb_dart_api_files
  dart_api_dl.c
  dart_api_dl.h
  dart_api.h
  dart_native_api.h
  dart_version.h
  internal/dart_api_dl_impl.h
)
set(_dcb_dart_api_source_ready TRUE)
foreach(_dcb_file IN LISTS _dcb_dart_api_files)
  if(NOT EXISTS "${_dcb_dart_api_source_dir}/${_dcb_file}")
    set(_dcb_dart_api_source_ready FALSE)
    break()
  endif()
endforeach()

if(DCB_DART_API_DIR)
  set(_dcb_dart_api_dir "${DCB_DART_API_DIR}")
elseif(_dcb_dart_api_source_ready)
  # Keep the existing monorepo workflow fast and source-compatible when the
  # ignored headers have already been downloaded by fetch_dart_api.cmake.
  set(_dcb_dart_api_dir "${_dcb_dart_api_source_dir}")
else()
  # A fetched source tree may be read-only or may not contain generated/ignored
  # files. Store the downloaded Dart API in the build tree instead of mutating
  # the dependency checkout.
  set(_dcb_dart_api_dir "${CMAKE_CURRENT_BINARY_DIR}/dart_api")
endif()

set(_dcb_dart_api_ready TRUE)
foreach(_dcb_file IN LISTS _dcb_dart_api_files)
  if(NOT EXISTS "${_dcb_dart_api_dir}/${_dcb_file}")
    set(_dcb_dart_api_ready FALSE)
    break()
  endif()
endforeach()

if(NOT _dcb_dart_api_ready)
  if(NOT DCB_FETCH_DART_API)
    message(FATAL_ERROR
      "Dart API DL headers are missing from '${_dcb_dart_api_dir}'. "
      "Run cmake -P cmake/fetch_dart_api.cmake, set DCB_DART_API_DIR, "
      "or keep DCB_FETCH_DART_API=ON.")
  endif()
  message(STATUS
    "dart_cpp_bridge: fetching Dart API DL headers into ${_dcb_dart_api_dir}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}"
      "-DDCB_DART_API_OUT=${_dcb_dart_api_dir}"
      "-DDCB_DART_API_VERSION=${DCB_DART_API_VERSION}"
      -P "${CMAKE_CURRENT_LIST_DIR}/cmake/fetch_dart_api.cmake"
    RESULT_VARIABLE _dcb_dart_api_result
  )
  if(NOT _dcb_dart_api_result EQUAL 0)
    message(FATAL_ERROR
      "Failed to fetch the Dart API DL headers (exit code "
      "${_dcb_dart_api_result}).")
  endif()
endif()

set(DCB_DART_API_DIR "${_dcb_dart_api_dir}")

# --- Base runtime (STATIC library, downstream links with WHOLE_ARCHIVE) ---
# Migration note (feat/stdexec-migration): src/cbridge.cpp has been re-ported to
# std::exec (stdexec starts_on + exec::task, see docs/cpp26_executor_model_usage.md)
# and is built again. The old ForeignExecutor mechanism (foreign_executor.hpp /
# foreign_runtime.h / src/foreign_runtime.cpp) has been removed: foreign event
# loops now plug in via a plain stdexec scheduler (see
# examples/foreign_runtime_demo/uv_scheduler.hpp).
add_library(dcb_runtime STATIC
  src/runtime/runtime.cpp
  src/runtime/object_handle.cpp
  src/cbridge.cpp
  src/dcb_codec.c
  src/ffi_entry.cpp
  ${DCB_DART_API_DIR}/dart_api_dl.c
)
set_target_properties(dcb_runtime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF
  CXX_STANDARD_REQUIRED ON
)
target_compile_features(dcb_runtime PUBLIC cxx_std_20)

target_include_directories(dcb_runtime
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${DCB_DART_API_DIR}
)

target_compile_definitions(dcb_runtime PUBLIC DART_CPP_BRIDGE_BUILD)
if(DCB_USE_BOOST_ASIO)
  # Keep the namespace selection on the public runtime target as well as on
  # the internal Asio interface. This remains correct when the parent supplied
  # an existing/imported `asio_iface` that cannot be modified here.
  target_compile_definitions(dcb_runtime PUBLIC DCB_USE_BOOST_ASIO)
endif()
# stdexec::task's task_scheduler preallocates a fixed-size operation state
# (STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE, default 72 bytes) to schedule work
# on the task's home scheduler. Storing the asio::post operation state of
# dcb::IoContextScheduler needs more than 72 bytes (measured: between 128
# and 256), so raise the cap. PUBLIC so downstream business code and
# generated wire dispatch see the same value.
target_compile_definitions(dcb_runtime PUBLIC STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE=256)
target_link_libraries(dcb_runtime PUBLIC STDEXEC::stdexec STDEXEC::asioexec asio_iface MPMCQueue)

if(WIN32)
  target_compile_definitions(dcb_runtime PUBLIC _WIN32_WINNT=0x0A00)
  if(MSVC)
    # MSVC / clang-cl style flag.
    target_compile_options(dcb_runtime PRIVATE /utf-8)
  else()
    # GNU-style compilers (MSYS2 clang/gcc): UTF-8 source/exec charset.
    target_compile_options(dcb_runtime PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
  endif()
endif()

# Alias for downstream users
add_library(dart_cpp_bridge::runtime ALIAS dcb_runtime)

# --- Optional: build the runtime directly as a SHARED library ---
# Used by the Native Assets build hook (dart/hook/build.dart) to produce a
# self-contained dart_cpp_bridge.{dll,so,dylib} whose dcb_* symbols are exported
# (DCB_API -> dllexport because DART_CPP_BRIDGE_BUILD is defined) and loadable
# via @Native. Downstream add_subdirectory users keep the STATIC + WHOLE_ARCHIVE
# flow; this stays OFF by default so it is not triggered for them.
option(DCB_BUILD_SHARED "Build dart_cpp_bridge runtime as a shared library" OFF)
if(DCB_BUILD_SHARED)
  add_library(dcb_shared SHARED
    src/runtime/runtime.cpp
    src/runtime/object_handle.cpp
    src/cbridge.cpp
    src/dcb_codec.c
    src/ffi_entry.cpp
    ${DCB_DART_API_DIR}/dart_api_dl.c
  )
  set_target_properties(dcb_shared PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_STANDARD_REQUIRED ON
  )
  target_compile_features(dcb_shared PUBLIC cxx_std_20)
  target_include_directories(dcb_shared
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${DCB_DART_API_DIR}
  )
  target_compile_definitions(dcb_shared PRIVATE DART_CPP_BRIDGE_BUILD)
  if(DCB_USE_BOOST_ASIO)
    target_compile_definitions(dcb_shared PRIVATE DCB_USE_BOOST_ASIO)
  endif()
  target_compile_definitions(dcb_shared PUBLIC STDEXEC_TASK_SCHEDULE_OPSTATE_SIZE=256)
  target_link_libraries(dcb_shared PRIVATE asio_iface MPMCQueue STDEXEC::stdexec STDEXEC::asioexec)
  if(WIN32)
    target_compile_definitions(dcb_shared PRIVATE _WIN32_WINNT=0x0A00)
    if(MSVC)
      # MSVC / clang-cl style flag.
      target_compile_options(dcb_shared PRIVATE /utf-8)
      # Use static CRT (/MT) to avoid depending on the system MSVCP140.dll.
      # The system copy (14.00, VS 2015) is incompatible with MSVC 19.51 code
      # when called via Dart @Native — std::mutex::lock() crashes in mtx_do_lock.
      set_property(TARGET dcb_shared PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
      # GNU-style compilers (MSYS2 clang/gcc): UTF-8 source/exec charset.
      target_compile_options(dcb_shared PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endif()
  endif()
  set_target_properties(dcb_shared PROPERTIES OUTPUT_NAME dart_cpp_bridge)
endif()
