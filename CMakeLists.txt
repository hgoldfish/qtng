cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(qtng LANGUAGES C CXX ASM VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CheckFunctionExists)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libressl/CMakeLists.txt")
    message(STATUS "Found libressl/ subdirectory, building bundled LibreSSL")
    add_subdirectory(libressl)
    set(QTNG_SSL_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/libressl/include")
    set(QTNG_SSL_LIBRARIES crypto ssl)
else()
    message(STATUS "libressl/ not found, using system OpenSSL")
    find_package(OpenSSL 1.1.0 REQUIRED)
    set(QTNG_SSL_INCLUDE ${OPENSSL_INCLUDE_DIR})
    set(QTNG_SSL_LIBRARIES OpenSSL::SSL OpenSSL::Crypto)
endif()

find_package(ZLIB REQUIRED)

set(UTILS_SRC
    src/utils/logging.cpp
    src/utils/string_utils.cpp
    src/utils/punycode.cpp
    src/utils/datetime.cpp
    src/utils/random.cpp
    src/utils/url.cpp
    src/utils/mime.cpp
)

set(QTNG_SRC
    ${UTILS_SRC}
    src/socket.cpp
    src/eventloop.cpp
    src/coroutine.cpp
    src/locks.cpp
    src/coroutine_utils.cpp
    src/socket_utils.cpp
    src/io_utils.cpp
    src/http.cpp
    src/http_utils.cpp
    src/http_proxy.cpp
    src/http_cookie.cpp
    src/socks5_proxy.cpp
    src/msgpack.cpp
    src/data_channel.cpp
    src/multi_stream.cpp
    src/hostaddress.cpp
    src/gzip.cpp
    src/socket_server.cpp
    src/httpd.cpp
    src/httpd2.cpp
    src/socks5_server.cpp
    src/websocket.cpp
    src/udp.cpp
    src/kcp.cpp
    src/kcp/ikcp.c
    src/kcp/ikcp.h
    src/network_interface/network_interface.cpp
)

set(QTNG_PUBLIC_INCLUDE
    include/qtng.h
    include/qtng/coroutine.h
    include/qtng/socket.h
    include/qtng/eventloop.h
    include/qtng/locks.h
    include/qtng/coroutine_utils.h
    include/qtng/http.h
    include/qtng/httpd.h
    include/qtng/socket_utils.h
    include/qtng/io_utils.h
    include/qtng/socket_server.h
    include/qtng/http_utils.h
    include/qtng/http_proxy.h
    include/qtng/http_cookie.h
    include/qtng/socks5_proxy.h
    include/qtng/deferred.h
    include/qtng/qtng.h
    include/qtng/msgpack.h
    include/qtng/data_channel.h
    include/qtng/multi_stream.h
    include/qtng/udp.h
    include/qtng/hostaddress.h
    include/qtng/network_interface.h
    include/qtng/gzip.h
    include/qtng/websocket.h
)

set(QTNG_PRIVATE_INCLUDE
    include/qtng/private/eventloop_p.h
    include/qtng/private/coroutine_p.h
    include/qtng/private/coroutine_utils_p.h
    include/qtng/private/socket_p.h
    include/qtng/private/kcp.h
    include/qtng/private/http_p.h
    include/qtng/private/hostaddress_p.h
    include/qtng/private/network_interface_p.h
)

set(QTNG_CRYPTO_SRC
    src/ssl.cpp
    src/random.cpp
    src/md.cpp
    src/pkey.cpp
    src/cipher.cpp
    src/certificate.cpp
    src/qasn1element.cpp
    src/noise.cpp
)

set(QTNG_CRYPTO_INCLUDE
    include/qtcrypto.h
    include/qtng/qtcrypto.h
    include/qtng/crypto.h
    include/qtng/ssl.h
    include/qtng/md.h
    include/qtng/random.h
    include/qtng/cipher.h
    include/qtng/pkey.h
    include/qtng/certificate.h
    include/qtng/noise.h
)

set(QTNG_CRYPTO_PRIVATE_INCLUDE
    include/qtng/private/crypto_p.h
    include/qtng/private/qasn1element.h
)

set(QTNG_LMDB_SRC
    src/lmdb.cpp
    src/liblmdb/midl.c
    src/liblmdb/midl.h
    src/liblmdb/mdb.c
    src/liblmdb/lmdb.h
)

set(QTNG_LMDB_INCLUDE
    include/qtng/lmdb.h
)

set(QTNG_UTILS_INCLUDE
    include/qtng/utils/platform.h
    include/qtng/utils/logging.h
    include/qtng/utils/string_utils.h
    include/qtng/utils/punycode.h
    include/qtng/utils/datetime.h
    include/qtng/utils/random.h
    include/qtng/utils/url.h
    include/qtng/utils/mime.h
    include/qtng/utils/shared_mutex_compat.h
    include/qtng/utils/thread_local.h
)

if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    set(OS_DEPENDENDED_SRC
        src/socket_win.cpp
        src/coroutine_win.cpp
        src/network_interface/network_interface_win.cpp
    )
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Android")
    if(${CMAKE_ANDROID_ARCH_ABI} STREQUAL "arm64-v8a")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_arm64_aapcs_elf_gas.S
            src/context/asm/make_arm64_aapcs_elf_gas.S
        )
    elseif(${CMAKE_ANDROID_ARCH_ABI} STREQUAL "armeabi-v7a")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_arm_aapcs_elf_gas.S
            src/context/asm/make_arm_aapcs_elf_gas.S
        )
    elseif(${CMAKE_ANDROID_ARCH_ABI} STREQUAL "mips")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_mips32_o32_elf_gas.S
            src/context/asm/make_mips32_o32_elf_gas.S
        )
    elseif(${CMAKE_ANDROID_ARCH_ABI} STREQUAL "mips64")
        message(FATAL_ERROR "Android for mips64 is not supported.")
    elseif(${CMAKE_ANDROID_ARCH_ABI} STREQUAL "x86")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_i386_sysv_elf_gas.S
            src/context/asm/make_i386_sysv_elf_gas.S
        )
    elseif(${CMAKE_ANDROID_ARCH_ABI} STREQUAL "x86_64")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_x86_64_sysv_elf_gas.S
            src/context/asm/make_x86_64_sysv_elf_gas.S
        )
    else()
        message(FATAL_ERROR "Android for ${CMAKE_ANDROID_ARCH_ABI} is not supported.")
    endif()
    list(APPEND OS_DEPENDENDED_SRC
        src/socket_unix.cpp
        src/coroutine_fcontext.cpp
        src/network_interface/network_interface_unix.cpp
    )
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(CMAKE_SYSTEM_PROCESSOR "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_x86_64_sysv_macho_gas.S
            src/context/asm/make_x86_64_sysv_macho_gas.S
        )
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "i386" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "i686")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_i386_sysv_macho_gas.S
            src/context/asm/make_i386_sysv_macho_gas.S
        )
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_arm64_aapcs_macho_gas.S
            src/context/asm/make_arm64_aapcs_macho_gas.S
        )
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_arm_aapcs_macho_gas.S
            src/context/asm/make_arm_aapcs_macho_gas.S
        )
    else()
        message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} is not supported.")
    endif()
    list(APPEND OS_DEPENDENDED_SRC
        src/socket_unix.cpp
        src/coroutine_fcontext.cpp
        src/network_interface/network_interface_unix.cpp
    )
elseif(UNIX)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_x86_64_sysv_elf_gas.S
            src/context/asm/make_x86_64_sysv_elf_gas.S
            src/coroutine_fcontext.cpp
        )
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "i386" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "i686")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_i386_sysv_elf_gas.S
            src/context/asm/make_i386_sysv_elf_gas.S
            src/coroutine_fcontext.cpp
        )
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_arm64_aapcs_elf_gas.S
            src/context/asm/make_arm64_aapcs_elf_gas.S
            src/coroutine_fcontext.cpp
        )
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "armv7" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_arm_aapcs_elf_gas.S
            src/context/asm/make_arm_aapcs_elf_gas.S
            src/coroutine_fcontext.cpp
        )
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "mips")
        set(OS_DEPENDENDED_SRC
            src/context/asm/jump_mips32_o32_elf_gas.S
            src/context/asm/make_mips32_o32_elf_gas.S
            src/coroutine_fcontext.cpp
        )
    else()
        set(OS_DEPENDENDED_SRC src/coroutine_unix.cpp)
    endif()
    list(APPEND OS_DEPENDENDED_SRC
        src/socket_unix.cpp
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND OS_DEPENDENDED_SRC src/network_interface/network_interface_linux.cpp)
    else()
        list(APPEND OS_DEPENDENDED_SRC src/network_interface/network_interface_unix.cpp)
    endif()
else()
    message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} is not supported.")
endif()

add_library(qtng STATIC
    ${QTNG_SRC}
    ${QTNG_PUBLIC_INCLUDE}
    ${QTNG_PRIVATE_INCLUDE}
    ${QTNG_CRYPTO_SRC}
    ${QTNG_CRYPTO_INCLUDE}
    ${QTNG_CRYPTO_PRIVATE_INCLUDE}
    ${QTNG_UTILS_INCLUDE}
    ${OS_DEPENDENDED_SRC}
    ${QTNG_LMDB_SRC}
    ${QTNG_LMDB_INCLUDE}
)

# Source layout above; build configuration below
set_target_properties(qtng PROPERTIES
    VERSION ${PROJECT_VERSION}
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(qtng PUBLIC cxx_std_11)

# Include directories: `include/` is on the CXX include path so both library
# sources and consumers use `#include "qtng/coroutine.h"` (or `#include "qtng.h"`).
target_include_directories(qtng
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${QTNG_SSL_INCLUDE}"
        ${ZLIB_INCLUDE_DIRS}
)

# Compile definitions
target_compile_definitions(qtng PUBLIC QTNG_HAVE_ZLIB)

# Platform-specific compile definitions
if(${CMAKE_SYSTEM_NAME} STREQUAL "Windows")
    target_compile_definitions(qtng PRIVATE QTNG_USE_WIN=1)
    target_sources(qtng PRIVATE src/eventloop_win.cpp)
elseif(UNIX)
    target_compile_definitions(qtng PRIVATE QTNG_USE_EV=1)
    target_compile_definitions(qtng PRIVATE EV_USE_4HEAP=1 EV_VERIFY=0 QTNG_EV_ASSERT=0)
    check_function_exists(kqueue HAVE_KQUEUE)
    check_function_exists(epoll_ctl HAVE_EPOLL)
    if(HAVE_EPOLL)
        message(STATUS "Use linux epoll() for libev.")
        target_compile_definitions(qtng PRIVATE EV_USE_EPOLL=1 EV_USE_EVENTFD=1 EV_USE_KQUEUE=0 EV_USE_POLL=0)
    elseif(HAVE_KQUEUE AND NOT CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
        message(STATUS "Use bsd kqueue() for libev.")
        target_compile_definitions(qtng PRIVATE EV_USE_EPOLL=0 EV_USE_EVENTFD=0 EV_USE_KQUEUE=1 EV_USE_POLL=0)
    else()
        message(STATUS "Use unix poll() for libev.")
        target_compile_definitions(qtng PRIVATE EV_USE_EPOLL=0 EV_USE_EVENTFD=0 EV_USE_KQUEUE=0 EV_USE_POLL=1)
    endif()
    target_sources(qtng PRIVATE src/ev/ev.c src/ev/ev.h src/eventloop_ev.cpp)
endif()

# Platform-specific link libraries
set(OS_EXTRA_LINK "")
if(UNIX AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    list(APPEND OS_EXTRA_LINK pthread)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND OS_EXTRA_LINK ws2_32)
endif()

target_link_libraries(qtng PUBLIC ${QTNG_SSL_LIBRARIES} ZLIB::ZLIB PRIVATE ${OS_EXTRA_LINK})

# Library configuration above; installation below
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(HAS_QTNG ON PARENT_SCOPE)
endif()

install(TARGETS qtng
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
# Umbrella headers: #include "qtng.h" / #include "qtcrypto.h"
install(FILES include/qtng.h include/qtcrypto.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
# Module headers: #include "qtng/coroutine.h", #include "qtng/utils/...", etc.
install(DIRECTORY include/qtng
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")

# Test configuration for internal development.
option(QTNG_BUILD_TESTS "Build unit tests (requires a C++17 compiler)" OFF)
if(QTNG_BUILD_TESTS)
    if(cxx_std_17 IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        enable_testing()
        add_subdirectory(tests)
    else()
        message(WARNING
            "QTNG_BUILD_TESTS is ON but the compiler does not support C++17 "
            "(cxx_std_17 not in CMAKE_CXX_COMPILE_FEATURES); skipping unit tests. "
            "The qtng library itself only requires C++11.")
    endif()
endif()
