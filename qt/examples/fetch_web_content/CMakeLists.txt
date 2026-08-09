cmake_minimum_required(VERSION 3.16)
project(fetch_web_content LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT TARGET qtnetworkng)
    include(${CMAKE_CURRENT_LIST_DIR}/../../CMakeLists.txt)
endif()

add_executable(fetch_web_content main.cpp)
target_link_libraries(fetch_web_content PRIVATE qtnetworkng)
