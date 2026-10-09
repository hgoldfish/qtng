cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(bandwidth LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BANDWIDTH_SERVER_SRC
    server_main.cpp
    common.h
)

set(BANDWIDTH_CLIENT_SRC
    client_main.cpp
    common.h
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

add_executable(bandwidth-server ${BANDWIDTH_SERVER_SRC})
target_link_libraries(bandwidth-server PRIVATE qtng)
set_target_properties(bandwidth-server PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(bandwidth-server PROPERTIES LINK_FLAGS_RELEASE -s)

add_executable(bandwidth-client ${BANDWIDTH_CLIENT_SRC})
target_link_libraries(bandwidth-client PRIVATE qtng)
set_target_properties(bandwidth-client PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(bandwidth-client PROPERTIES LINK_FLAGS_RELEASE -s)
