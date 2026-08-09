# Build Transmission libutp as a static library for interoperability tests only.
# Kept outside the submodule so we do not need a forked libutp tree.
# Included from tests/CMakeLists.txt when 3rdparty/libutp is initialized.

if(TARGET utp)
    return()
endif()

set(_QTNG_LIBUTP_DIR "${CMAKE_CURRENT_LIST_DIR}/libutp")
if(NOT EXISTS "${_QTNG_LIBUTP_DIR}/utp.h")
    message(FATAL_ERROR "3rdparty/libutp is empty; run: git submodule update --init 3rdparty/libutp")
endif()

set(_QTNG_LIBUTP_SOURCES
    ${_QTNG_LIBUTP_DIR}/utp_api.cpp
    ${_QTNG_LIBUTP_DIR}/utp_callbacks.cpp
    ${_QTNG_LIBUTP_DIR}/utp_hash.cpp
    ${_QTNG_LIBUTP_DIR}/utp_internal.cpp
    ${_QTNG_LIBUTP_DIR}/utp_packedsockaddr.cpp
    ${_QTNG_LIBUTP_DIR}/utp_utils.cpp
)

if(WIN32)
    list(APPEND _QTNG_LIBUTP_SOURCES ${_QTNG_LIBUTP_DIR}/libutp_inet_ntop.cpp)
endif()

add_library(utp STATIC ${_QTNG_LIBUTP_SOURCES})
target_include_directories(utp PUBLIC "${_QTNG_LIBUTP_DIR}")
set_target_properties(utp PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# Upstream Makefile uses -DPOSIX on Unix; MSVC projects use WIN32. Headers
# branch on those macros (SOCKADDR_STORAGE typedef, socket includes).
if(WIN32)
    target_compile_definitions(utp PRIVATE WIN32)
    target_link_libraries(utp PUBLIC ws2_32)
else()
    target_compile_definitions(utp PRIVATE POSIX)
endif()

if(UNIX AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(utp PUBLIC pthread)
endif()
if(UNIX)
    target_link_libraries(utp PUBLIC m)
endif()

unset(_QTNG_LIBUTP_DIR)
unset(_QTNG_LIBUTP_SOURCES)
