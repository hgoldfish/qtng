# Optional third-party sources (git submodules).
# Initialize with: git submodule update --init --recursive

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libutp/utp.h")
    include("${CMAKE_CURRENT_SOURCE_DIR}/libutp.cmake")
    set(QTNG_HAVE_LIBUTP ON PARENT_SCOPE)
endif()
