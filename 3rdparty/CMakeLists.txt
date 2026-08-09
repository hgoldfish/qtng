# Optional third-party sources (git submodules).
# Initialize with: git submodule update --init --recursive

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libutp/utp.h")
    add_subdirectory(libutp)
    set(QTNG_HAVE_LIBUTP ON PARENT_SCOPE)
endif()
