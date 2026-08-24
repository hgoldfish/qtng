cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(ssh-bbs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ssh-bbs needs the SSH module. When built standalone (add_subdirectory(../..))
# default it to ON; an explicit -DQTNG_WITH_SSH=... always wins.
if(NOT DEFINED QTNG_WITH_SSH)
    set(QTNG_WITH_SSH ON CACHE BOOL "Build SSH protocol (SshServer/SshClient)" FORCE)
endif()

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

add_executable(ssh-bbs main.cpp)
target_link_libraries(ssh-bbs PRIVATE qtng)
