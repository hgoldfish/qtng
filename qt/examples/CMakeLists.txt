cmake_minimum_required(VERSION 3.16)
project(qtng_qt_examples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT TARGET qtnetworkng)
    include(${CMAKE_CURRENT_LIST_DIR}/../CMakeLists.txt)
endif()

add_subdirectory(fetch_web_content)
add_subdirectory(include_isolation)
