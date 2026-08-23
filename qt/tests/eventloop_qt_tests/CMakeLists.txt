cmake_minimum_required(VERSION 3.14)
project(eventloop_qt_tests)

include("${CMAKE_CURRENT_LIST_DIR}/../../CMakeLists.txt")

find_package(Qt5 REQUIRED COMPONENTS Core)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(add_loop_test name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE "${QTNG_ROOT}/include")
    target_link_libraries(${name} PRIVATE qtnetworkng Qt5::Core)
endfunction()

add_loop_test(t_loop_basic)
add_loop_test(t_loop_spawn_before_loop)
add_loop_test(t_loop_spawn_after_loop)
add_loop_test(t_loop_identity)
add_loop_test(t_gui_freeze)
add_loop_test(t_gui_event_handler_block)
add_loop_test(t_coroutine_await_qt)
add_loop_test(t_network_io)
add_loop_test(t_bridge)
add_loop_test(t_use_eventloop_qt)
add_loop_test(t_use_eventloop_ev)
