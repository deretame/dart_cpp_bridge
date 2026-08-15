# patch_stdexec_any_allocator.cmake — applied via FetchContent PATCH_COMMAND.
#
# stdexec's `__any_allocator` converting constructor accesses the private
# member of another specialization of the same class template, which the C++
# standard permits ([class.access]) but MSVC does not implement (error C2248).
# The workaround is an explicit friend declaration; it is harmless on other
# compilers.
#
# Invoked by dart/native/CMakeLists.txt as:
#   ${CMAKE_COMMAND} -DSTDEXEC_SRC=<SOURCE_DIR> -P patch_stdexec_any_allocator.cmake
if(NOT DEFINED STDEXEC_SRC)
  message(FATAL_ERROR "patch_stdexec_any_allocator.cmake: STDEXEC_SRC must point at the stdexec source tree")
endif()

set(_hdr "${STDEXEC_SRC}/include/stdexec/__detail/__any_allocator.hpp")
if(NOT EXISTS "${_hdr}")
  message(FATAL_ERROR "patch_stdexec_any_allocator.cmake: ${_hdr} not found")
endif()

file(READ "${_hdr}" _content)

# Use bracket arguments instead of quoted multi-line strings.  CMake 4.4's
# list handling can otherwise turn the newlines in a quoted value into literal
# semicolons, corrupting the patched header.
set(_anchor [=[    __any_allocator() = default;
]=])
set(_inject [=[    __any_allocator() = default;

    // MSVC does not implement [class.access]: members of a class template
    // cannot access private members of other specializations of the same
    // template. The converting constructor below relies on that rule, so
    // declare the friendship explicitly.
    template <class>
    friend struct __any_allocator;
]=])

if(_content MATCHES "friend struct __any_allocator")
  message(STATUS "patch_stdexec_any_allocator: already applied, skipping")
elseif(NOT _content MATCHES "    __any_allocator\\(\\) = default;")
  message(FATAL_ERROR "patch_stdexec_any_allocator: anchor not found; stdexec layout changed?")
else()
  string(REPLACE "${_anchor}" "${_inject}" _content "${_content}")
  file(WRITE "${_hdr}" "${_content}")
  message(STATUS "patch_stdexec_any_allocator: applied friend declaration")
endif()
