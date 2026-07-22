# cmake -P cmake/fetch_dart_api.cmake
cmake_minimum_required(VERSION 3.20)

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(OUT "${ROOT}/third_party/dart_api")
set(BASE "https://raw.githubusercontent.com/dart-lang/sdk/3.5.0/runtime/include")

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
