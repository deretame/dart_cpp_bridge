# =============================================================================
# dcb_find_package.cmake — Locate dart_cpp_bridge and set up DCB_ROOT
# =============================================================================
#
# This module resolves the dart_cpp_bridge package location and sets:
#   DCB_PKG_PATH  — dart_cpp_bridge package root (contains native/, lib/, etc.)
#   DCB_ROOT      — dart_cpp_bridge/native (for add_subdirectory)
#
# Resolution priority:
#   1. DCB_PKG_PATH already set (monorepo / hook-provided) → validate & use
#   2. Resolve from ${DCB_PROJECT_ROOT}/.dart_tool/package_config.json
#
# Usage (monorepo / known path):
#   set(DCB_PKG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../dart")
#   include("${DCB_PKG_PATH}/native/cmake/dcb_find_package.cmake")
#   add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)
#
# Usage (external project, resolved from package_config.json):
#   set(DCB_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
#   list(APPEND CMAKE_MODULE_PATH "${DCB_PROJECT_ROOT}/<path-to>/cmake")
#   include(dcb_find_package)
#   add_subdirectory(${DCB_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/dcb_runtime)
#
# Input variables:
#   DCB_PROJECT_ROOT — Dart/Flutter project root (contains .dart_tool/).
#                      Defaults to CMAKE_CURRENT_SOURCE_DIR/.. if not set.
#   DCB_PKG_PATH     — (optional) pre-resolved dart_cpp_bridge package path.
# =============================================================================

include_guard(GLOBAL)

# Default project root: assume this CMakeLists is in <project>/native/
if(NOT DEFINED DCB_PROJECT_ROOT)
  get_filename_component(DCB_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
endif()

# --- Fast path: DCB_PKG_PATH already provided ---
if(DEFINED DCB_PKG_PATH AND NOT DCB_PKG_PATH STREQUAL "")
  if(NOT EXISTS "${DCB_PKG_PATH}/native/include")
    message(FATAL_ERROR
      "DCB_PKG_PATH='${DCB_PKG_PATH}' does not contain native/include.\n"
      "Verify the path points to the dart_cpp_bridge package root.")
  endif()
  set(DCB_ROOT "${DCB_PKG_PATH}/native")
  message(STATUS "dart_cpp_bridge package: ${DCB_PKG_PATH}")
  return()
endif()

# --- Resolve from package_config.json ---
set(_dcb_pkg_config "${DCB_PROJECT_ROOT}/.dart_tool/package_config.json")
if(NOT EXISTS "${_dcb_pkg_config}")
  message(FATAL_ERROR
    "Missing ${_dcb_pkg_config}\n"
    "Run 'dart pub get' first, or set DCB_PKG_PATH manually.")
endif()

file(READ "${_dcb_pkg_config}" _dcb_pkg_json)

# Find the dart_cpp_bridge entry in the packages array.
set(_dcb_idx -1)
string(JSON _dcb_count LENGTH "${_dcb_pkg_json}" "packages")
math(EXPR _dcb_last "${_dcb_count} - 1")
foreach(_i RANGE ${_dcb_last})
  string(JSON _dcb_name GET "${_dcb_pkg_json}" "packages" ${_i} "name")
  if(_dcb_name STREQUAL "dart_cpp_bridge")
    set(_dcb_idx ${_i})
    break()
  endif()
endforeach()

if(_dcb_idx EQUAL -1)
  message(FATAL_ERROR
    "dart_cpp_bridge not found in package_config.json.\n"
    "Add dart_cpp_bridge to your pubspec.yaml dependencies and run 'dart pub get'.")
endif()

# Extract rootUri and convert to filesystem path.
string(JSON _dcb_uri GET "${_dcb_pkg_json}" "packages" ${_dcb_idx} "rootUri")

if(_dcb_uri MATCHES "^file://")
  if(WIN32)
    string(REGEX REPLACE "^file:///" "" DCB_PKG_PATH "${_dcb_uri}")
  else()
    string(REGEX REPLACE "^file://" "" DCB_PKG_PATH "${_dcb_uri}")
  endif()
else()
  # Relative URI: resolved against .dart_tool/ directory.
  get_filename_component(DCB_PKG_PATH
    "${DCB_PROJECT_ROOT}/.dart_tool/${_dcb_uri}" ABSOLUTE)
endif()

# Validate.
if(NOT EXISTS "${DCB_PKG_PATH}/native/include")
  message(FATAL_ERROR
    "Resolved dart_cpp_bridge path '${DCB_PKG_PATH}' is invalid "
    "(missing native/include).\n"
    "Try: dart pub get")
endif()

set(DCB_ROOT "${DCB_PKG_PATH}/native")
message(STATUS "dart_cpp_bridge package: ${DCB_PKG_PATH}")
