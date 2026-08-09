cmake_minimum_required(VERSION 3.16)
project(qtng_include_isolation_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT TARGET qtnetworkng)
    include(${CMAKE_CURRENT_LIST_DIR}/../../CMakeLists.txt)
endif()

# This target must fail to compile: core headers are not on the public include path.
add_executable(include_isolation_fail main.cpp)
target_link_libraries(include_isolation_fail PRIVATE qtnetworkng)
