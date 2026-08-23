cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(noise-udp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

add_executable(noise-udp main.cpp)
target_link_libraries(noise-udp PRIVATE qtng)
