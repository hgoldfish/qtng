# Qt binding for qtng 2.0 — include from parent projects:
#   include(/path/to/qtng/qt/CMakeLists.txt)
#   target_link_libraries(myapp PRIVATE qtnetworkng)

cmake_minimum_required(VERSION 3.14)

set(QTNG_QT_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(QTNG_ROOT "${QTNG_QT_DIR}/..")

if(NOT TARGET qtng)
    add_subdirectory("${QTNG_ROOT}" "${CMAKE_BINARY_DIR}/qtng-core" EXCLUDE_FROM_ALL)
    # Core is only linked privately by qtnetworkng; compile it into namespace qtng_core so
    # bridge TUs (core_access.h) link against the same mangled symbols as the Qt API layer.
    target_compile_definitions(qtng PRIVATE qtng=qtng_core)
endif()

find_package(Qt5 COMPONENTS Core QUIET)
if(Qt5_FOUND)
    set(QTNG_QT_VERSION_MAJOR 5)
    set(QTNG_QT_LIB Qt5::Core)
    set(QTNG_QT_CXX_STANDARD 11)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core)
    set(QTNG_QT_VERSION_MAJOR 6)
    set(QTNG_QT_LIB Qt6::Core)
    set(QTNG_QT_CXX_STANDARD 17)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD ${QTNG_QT_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(UNIX AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(QTNG_QT_EXTRA_LIBS pthread)
endif()

if(WIN32)
    set(QTNG_QT_EXTRA_LIBS ws2_32)
endif()

if(UNIX AND NOT WIN32)
    set(QTNG_QT_COMPILE_DEFS QTNETWOKRNG_USE_EV=1)
elseif(WIN32)
    set(QTNG_QT_COMPILE_DEFS QTNETWORKNG_USE_WIN=1)
endif()

set(QTNG_QT_PUBLIC_HEADERS
    ${QTNG_QT_DIR}/include/config.h
    ${QTNG_QT_DIR}/include/qtnetworkng.h
    ${QTNG_QT_DIR}/include/qtng.h
    ${QTNG_QT_DIR}/include/coroutine.h
    ${QTNG_QT_DIR}/include/deferred.h
    ${QTNG_QT_DIR}/include/eventloop.h
    ${QTNG_QT_DIR}/include/locks.h
    ${QTNG_QT_DIR}/include/coroutine_utils.h
    ${QTNG_QT_DIR}/include/hostaddress.h
    ${QTNG_QT_DIR}/include/network_interface.h
    ${QTNG_QT_DIR}/include/socket.h
    ${QTNG_QT_DIR}/include/socket_utils.h
    ${QTNG_QT_DIR}/include/io_utils.h
    ${QTNG_QT_DIR}/include/http_utils.h
    ${QTNG_QT_DIR}/include/http_cookie.h
    ${QTNG_QT_DIR}/include/http.h
    ${QTNG_QT_DIR}/include/http_proxy.h
    ${QTNG_QT_DIR}/include/socks5_proxy.h
    ${QTNG_QT_DIR}/include/httpd.h
    ${QTNG_QT_DIR}/include/socket_server.h
    ${QTNG_QT_DIR}/include/websocket.h
    ${QTNG_QT_DIR}/include/data_channel.h
    ${QTNG_QT_DIR}/include/msgpack.h
    ${QTNG_QT_DIR}/include/bencode.h
    ${QTNG_QT_DIR}/include/kademlia.h
    ${QTNG_QT_DIR}/include/multi_stream.h
    ${QTNG_QT_DIR}/include/udp.h
    ${QTNG_QT_DIR}/include/kcp.h
    ${QTNG_QT_DIR}/include/kcp_base.h
    ${QTNG_QT_DIR}/include/lmdb.h
    ${QTNG_QT_DIR}/include/gzip.h
    ${QTNG_QT_DIR}/include/pool.h
    ${QTNG_QT_DIR}/include/qtcrypto.h
    ${QTNG_QT_DIR}/include/crypto.h
    ${QTNG_QT_DIR}/include/ssl.h
    ${QTNG_QT_DIR}/include/random.h
    ${QTNG_QT_DIR}/include/md.h
    ${QTNG_QT_DIR}/include/cipher.h
    ${QTNG_QT_DIR}/include/pkey.h
    ${QTNG_QT_DIR}/include/certificate.h
    ${QTNG_QT_DIR}/include/noise.h
)

set(QTNG_QT_SOURCES
    ${QTNG_QT_DIR}/src/bridge/coroutine_registry.cpp
    ${QTNG_QT_DIR}/src/bridge/stream_bridge.cpp
    ${QTNG_QT_DIR}/src/bridge/io_bridge.cpp
    ${QTNG_QT_DIR}/src/coroutine.cpp
    ${QTNG_QT_DIR}/src/eventloop.cpp
    ${QTNG_QT_DIR}/src/eventloop_qt.cpp
    ${QTNG_QT_DIR}/src/locks.cpp
    ${QTNG_QT_DIR}/src/hostaddress.cpp
    ${QTNG_QT_DIR}/src/network_interface.cpp
    ${QTNG_QT_DIR}/src/socket.cpp
    ${QTNG_QT_DIR}/src/socket_utils.cpp
    ${QTNG_QT_DIR}/src/io_utils.cpp
    ${QTNG_QT_DIR}/src/coroutine_utils.cpp
    ${QTNG_QT_DIR}/src/http_utils.cpp
    ${QTNG_QT_DIR}/src/http_cookie.cpp
    ${QTNG_QT_DIR}/src/http.cpp
    ${QTNG_QT_DIR}/src/http_proxy.cpp
    ${QTNG_QT_DIR}/src/socks5_proxy.cpp
    ${QTNG_QT_DIR}/src/httpd.cpp
    ${QTNG_QT_DIR}/src/socket_server.cpp
    ${QTNG_QT_DIR}/src/websocket.cpp
    ${QTNG_QT_DIR}/src/data_channel.cpp
    ${QTNG_QT_DIR}/src/msgpack.cpp
    ${QTNG_QT_DIR}/src/bencode.cpp
    ${QTNG_QT_DIR}/src/kademlia.cpp
    ${QTNG_QT_DIR}/src/multi_stream.cpp
    ${QTNG_QT_DIR}/src/udp.cpp
    ${QTNG_QT_DIR}/src/lmdb.cpp
    ${QTNG_QT_DIR}/src/gzip.cpp
    ${QTNG_QT_DIR}/src/pool.cpp
    ${QTNG_QT_DIR}/src/ssl.cpp
    ${QTNG_QT_DIR}/src/random.cpp
    ${QTNG_QT_DIR}/src/md.cpp
    ${QTNG_QT_DIR}/src/cipher.cpp
    ${QTNG_QT_DIR}/src/pkey.cpp
    ${QTNG_QT_DIR}/src/certificate.cpp
    ${QTNG_QT_DIR}/src/noise.cpp
)

add_library(qtnetworkng STATIC
    ${QTNG_QT_PUBLIC_HEADERS}
    ${QTNG_QT_SOURCES}
    ${QTNG_QT_DIR}/src/eventloop_qt_p.h
)

target_include_directories(qtnetworkng
    PUBLIC
        "${QTNG_QT_DIR}/include"
    PRIVATE
        "${QTNG_QT_DIR}/src"
        "${QTNG_ROOT}/include"
)

target_compile_definitions(qtnetworkng
    PUBLIC
        QTNG_HAVE_ZLIB
        ${QTNG_QT_COMPILE_DEFS}
)

target_link_libraries(qtnetworkng
    PUBLIC ${QTNG_QT_LIB}
    PRIVATE qtng ${QTNG_QT_EXTRA_LIBS}
)

target_compile_features(qtnetworkng PUBLIC cxx_std_${QTNG_QT_CXX_STANDARD})

if(NOT CMAKE_SOURCE_DIR STREQUAL QTNG_ROOT)
    set(HAS_QTNETWORKNG ON PARENT_SCOPE)
endif()
