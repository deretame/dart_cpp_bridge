# Top-level FetchContent entry point for dart_cpp_bridge.
#
# The Dart package keeps its native implementation under dart/native. A
# top-level wrapper lets consumers use the conventional form below without
# knowing the monorepo layout:
#
#   include(FetchContent)
#   FetchContent_Declare(dart_cpp_bridge
#     GIT_REPOSITORY https://github.com/deretame/dart_cpp_bridge.git
#     GIT_TAG v2.2.0)
#   FetchContent_MakeAvailable(dart_cpp_bridge)
#   target_link_libraries(app PRIVATE
#     $<LINK_LIBRARY:WHOLE_ARCHIVE,dart_cpp_bridge::runtime>)

cmake_minimum_required(VERSION 3.25)
project(dart_cpp_bridge LANGUAGES C CXX)

add_subdirectory(dart/native)
