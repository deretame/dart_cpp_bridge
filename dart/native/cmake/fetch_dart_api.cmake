# cmake -P dart/native/cmake/fetch_dart_api.cmake
cmake_minimum_required(VERSION 3.20)

# This script lives in dart/native/cmake/. When called by the native
# CMakeLists, the output is placed in the build tree so FetchContent source
# checkouts are not modified. Direct invocation keeps the historical output
# path under dart/native/third_party/dart_api/.
if(NOT DEFINED DCB_DART_API_OUT OR DCB_DART_API_OUT STREQUAL "")
  set(NATIVE_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
  set(OUT "${NATIVE_DIR}/third_party/dart_api")
else()
  set(OUT "${DCB_DART_API_OUT}")
endif()

if(NOT DEFINED DCB_DART_API_VERSION OR DCB_DART_API_VERSION STREQUAL "")
  set(DCB_DART_API_VERSION "3.10.0")
endif()
set(BASE "https://raw.githubusercontent.com/dart-lang/sdk/${DCB_DART_API_VERSION}/runtime/include")

file(MAKE_DIRECTORY "${OUT}")
file(MAKE_DIRECTORY "${OUT}/internal")

set(FILES
  dart_api_dl.h
  dart_api_dl.c
  dart_api.h
  dart_native_api.h
  dart_version.h
  internal/dart_api_dl_impl.h
)

foreach(f ${FILES})
  set(url "${BASE}/${f}")
  set(dst "${OUT}/${f}")
  message(STATUS "GET ${url}")
  file(DOWNLOAD "${url}" "${dst}" SHOW_PROGRESS STATUS st)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "download failed: ${url} (${st})")
  endif()
endforeach()

message(STATUS "dart_api headers ready at ${OUT}")
