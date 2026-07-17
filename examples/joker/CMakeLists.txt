cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(jokerproxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JOKER_SERVER_SRC
    server_main.cpp
    joker_server.cpp
    joker_server.h
)

set(JOKER_CLIENT_SRC
    client_main.cpp
    joker_client.cpp
    joker_client.h
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

add_executable(joker-server ${JOKER_SERVER_SRC})
target_link_libraries(joker-server PRIVATE qtng)
set_target_properties(joker-server PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(joker-server PROPERTIES LINK_FLAGS_RELEASE -s)

add_executable(joker-client ${JOKER_CLIENT_SRC})
target_link_libraries(joker-client PRIVATE qtng)
set_target_properties(joker-client PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(joker-client PROPERTIES LINK_FLAGS_RELEASE -s)

add_executable(test-socks5-google test_socks5_google.cpp)
target_link_libraries(test-socks5-google PRIVATE qtng)
