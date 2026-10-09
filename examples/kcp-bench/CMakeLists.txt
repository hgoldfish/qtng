cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(kcp-bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(KCP_BENCH_SRC
    bench_main.cpp
    bench_stats.cpp
    bench_stats.h
    sim_link.cpp
    sim_link.h
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

add_executable(kcp-bench ${KCP_BENCH_SRC})
target_link_libraries(kcp-bench PRIVATE qtng)
target_include_directories(kcp-bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
