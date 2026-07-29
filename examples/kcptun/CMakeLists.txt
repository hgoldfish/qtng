cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(kcptun LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KCPTUN_COMMON_SRC
    common.cpp
    common.h
)

set(KCPTUN_SERVER_SRC
    server_main.cpp
    ${KCPTUN_COMMON_SRC}
)

set(KCPTUN_CLIENT_SRC
    client_main.cpp
    ${KCPTUN_COMMON_SRC}
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

add_executable(kcptun-server ${KCPTUN_SERVER_SRC})
target_link_libraries(kcptun-server PRIVATE qtng)
set_target_properties(kcptun-server PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(kcptun-server PROPERTIES LINK_FLAGS_RELEASE -s)

add_executable(kcptun-client ${KCPTUN_CLIENT_SRC})
target_link_libraries(kcptun-client PRIVATE qtng)
set_target_properties(kcptun-client PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(kcptun-client PROPERTIES LINK_FLAGS_RELEASE -s)
