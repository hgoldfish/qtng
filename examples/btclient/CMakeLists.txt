cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(btclient LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 COMPONENTS Widgets QUIET)
if(Qt5_FOUND)
    set(BTCLIENT_QT_LIB Qt5::Widgets)
else()
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    set(BTCLIENT_QT_LIB Qt6::Widgets)
    set(CMAKE_CXX_STANDARD 17)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

add_executable(btclient
    main.cpp
    mainwindow.cpp
    mainwindow.h
)

target_link_libraries(btclient PRIVATE qtng ${BTCLIENT_QT_LIB})
