cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(qtng_rpc LANGUAGES CXX)

# qtng_rpc is a standalone add_subdirectory library. It requires the qtng target.
# If qtng has not been configured yet (e.g. a consumer only does
# add_subdirectory(qtng/rpc)), pull in the parent qtng project first.
if(NOT TARGET qtng)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}/../qtng")
endif()

add_library(qtng_rpc STATIC
    src/base.cpp
    src/value.cpp
    src/peer.cpp
    src/rpc.cpp
    src/sendfile.cpp
    src/senddir.cpp
    include/qtng/rpc.h
    include/qtng/rpc/base.h
    include/qtng/rpc/value.h
    include/qtng/rpc/header.h
    include/qtng/rpc/registration.h
    include/qtng/rpc/peer.h
    include/qtng/rpc/rpc.h
    include/qtng/rpc/sendfile.h
    include/qtng/rpc/senddir.h
)

set_target_properties(qtng_rpc PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(qtng_rpc PUBLIC cxx_std_17)

target_include_directories(qtng_rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(qtng_rpc PUBLIC qtng)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HAS_QTNG_RPC ON PARENT_SCOPE)
endif()

install(TARGETS qtng_rpc
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/qtng/rpc.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/qtng/rpc
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
