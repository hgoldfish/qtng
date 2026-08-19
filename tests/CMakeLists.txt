cmake_minimum_required(VERSION 3.14)

# Catch2 is never fetched from the network and is not vendored in this repo,
# to avoid supply-chain risk. It must be installed system-wide (Catch2 v3 or
# newer) and is discovered with find_package. The qtng library itself does not
# depend on Catch2; it is only required when QTNG_BUILD_TESTS=ON.
find_package(Catch2 3 QUIET)
if(NOT Catch2_FOUND)
    message(FATAL_ERROR
        "Catch2 (v3 or newer) was not found, but QTNG_BUILD_TESTS=ON needs "
        "it to build the unit tests. Catch2 is intentionally not downloaded "
        "automatically, to avoid supply-chain risk. Install it yourself:\n"
        "  Debian/Ubuntu (bookworm+): sudo apt install catch2\n"
        "  Fedora:                     sudo dnf install catch2-devel\n"
        "  Arch Linux:                 sudo pacman -S catch2\n"
        "  macOS (Homebrew):           brew install catch2\n"
        "  vcpkg:                      vcpkg install catch2\n"
        "  From source:                https://github.com/catchorg/Catch2\n"
        "If Catch2 is installed to a non-default prefix, point CMake at it "
        "with -DCMAKE_PREFIX_PATH=... (or -DCatch2_ROOT=...).\n"
        "If you do not need the unit tests, leave QTNG_BUILD_TESTS=OFF; the "
        "qtng library builds without Catch2.")
endif()

# find_package(Catch2) already puts Catch.cmake on the module path for a
# system install; this is a belt-and-braces fallback for unusual installs.
if(Catch2_MODULE_PATH)
    list(APPEND CMAKE_MODULE_PATH ${Catch2_MODULE_PATH})
endif()

include(CTest)
include(Catch)

set(QTNG_TEST_SUPPORT_SRC
    ${CMAKE_SOURCE_DIR}/src/utils/logging.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/string_utils.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/punycode.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/datetime.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/random.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/url.cpp
    ${CMAKE_SOURCE_DIR}/src/utils/mime.cpp
    ${CMAKE_SOURCE_DIR}/src/hostaddress.cpp
    ${CMAKE_SOURCE_DIR}/src/http_cookie.cpp
)

add_library(qtng_test_support STATIC ${QTNG_TEST_SUPPORT_SRC})
target_include_directories(qtng_test_support
    PUBLIC "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}"
)
set_target_properties(qtng_test_support PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(qtng_test_support PUBLIC ws2_32)
elseif(UNIX AND NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(qtng_test_support PUBLIC pthread)
endif()

add_executable(qtng_tests
    test_string_utils.cpp
    test_websocket_mask_simd.cpp
    test_url.cpp
    test_datetime.cpp
    test_mime.cpp
    test_random.cpp
    test_platform.cpp
    test_thread_local.cpp
    test_hostaddress.cpp
    test_http_cookie.cpp
    test_logging.cpp
    test_deferred.cpp
)

target_link_libraries(qtng_tests PRIVATE qtng_test_support Catch2::Catch2WithMain)
target_include_directories(qtng_tests PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_tests PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

catch_discover_tests(qtng_tests)

# Always exercise the software MessageDigest backend (md_fallback.cpp), even when
# the main qtng library is linked against OpenSSL/LibreSSL.
add_executable(qtng_test_md_fallback
    test_md.cpp
    ${CMAKE_SOURCE_DIR}/src/md_fallback.cpp
)
target_compile_definitions(qtng_test_md_fallback PRIVATE QTNG_NO_CRYPTO)
target_link_libraries(qtng_test_md_fallback PRIVATE Catch2::Catch2WithMain)
target_include_directories(qtng_test_md_fallback PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_md_fallback PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
catch_discover_tests(qtng_test_md_fallback)

# Same vectors against the OpenSSL/LibreSSL-backed MessageDigest in libqtng.
# Skip when QTNG_NO_CRYPTO — that would just re-run the software path above.
if(QTNG_HAVE_CRYPTO)
    add_executable(qtng_test_md
        test_md.cpp
    )
    target_link_libraries(qtng_test_md PRIVATE qtng Catch2::Catch2WithMain)
    target_include_directories(qtng_test_md PRIVATE "${CMAKE_SOURCE_DIR}/include")
    set_target_properties(qtng_test_md PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    catch_discover_tests(qtng_test_md)
endif()

if(QTNG_WITH_HTTP2)
    add_executable(qtng_test_http2
        test_http2.cpp
    )
    target_link_libraries(qtng_test_http2 PRIVATE qtng Catch2::Catch2WithMain)
    target_include_directories(qtng_test_http2 PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}")
    set_target_properties(qtng_test_http2 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    catch_discover_tests(qtng_test_http2)
endif()

add_executable(qtng_test_msgpack
    test_msgpack_variant.cpp
)
target_link_libraries(qtng_test_msgpack PRIVATE qtng Catch2::Catch2WithMain)
target_include_directories(qtng_test_msgpack PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_msgpack PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

catch_discover_tests(qtng_test_msgpack)

add_executable(qtng_test_multi_stream
    test_multi_stream.cpp
)
target_link_libraries(qtng_test_multi_stream PRIVATE qtng Catch2::Catch2WithMain)
target_include_directories(qtng_test_multi_stream PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_multi_stream PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

catch_discover_tests(qtng_test_multi_stream)

add_executable(qtng_test_bencode
    test_bencode.cpp
)
target_link_libraries(qtng_test_bencode PRIVATE qtng Catch2::Catch2WithMain)
target_include_directories(qtng_test_bencode PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_bencode PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

catch_discover_tests(qtng_test_bencode)

if(QTNG_HAVE_CRYPTO)
    add_executable(qtng_test_certificate
        test_certificate.cpp
    )
    target_link_libraries(qtng_test_certificate PRIVATE qtng Catch2::Catch2WithMain)
    target_include_directories(qtng_test_certificate PRIVATE "${CMAKE_SOURCE_DIR}/include")
    set_target_properties(qtng_test_certificate PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    catch_discover_tests(qtng_test_certificate)

    add_executable(qtng_test_noise
        test_noise.cpp
    )
    target_link_libraries(qtng_test_noise PRIVATE qtng Catch2::Catch2WithMain)
    target_include_directories(qtng_test_noise PRIVATE "${CMAKE_SOURCE_DIR}/include")
    set_target_properties(qtng_test_noise PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    catch_discover_tests(qtng_test_noise)
endif()

add_executable(qtng_test_mqtt
    test_mqtt.cpp
)
target_link_libraries(qtng_test_mqtt PRIVATE qtng Catch2::Catch2WithMain)
target_include_directories(qtng_test_mqtt PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_mqtt PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

catch_discover_tests(qtng_test_mqtt)

add_executable(qtng_test_kademlia
    test_kademlia.cpp
)
target_link_libraries(qtng_test_kademlia PRIVATE qtng Catch2::Catch2WithMain)
target_include_directories(qtng_test_kademlia PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_kademlia PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

catch_discover_tests(qtng_test_kademlia)

add_executable(qtng_test_bt
    test_bt.cpp
)
target_link_libraries(qtng_test_bt PRIVATE qtng Catch2::Catch2WithMain)
target_include_directories(qtng_test_bt PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_bt PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

catch_discover_tests(qtng_test_bt)

add_executable(qtng_test_utp
    test_utp.cpp
)
target_link_libraries(qtng_test_utp PRIVATE qtng Catch2::Catch2WithMain)
target_include_directories(qtng_test_utp PRIVATE "${CMAKE_SOURCE_DIR}/include")
set_target_properties(qtng_test_utp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
catch_discover_tests(qtng_test_utp)

if(QTNG_WITH_QUIC)
    add_executable(qtng_test_quic
        test_quic.cpp
    )
    target_link_libraries(qtng_test_quic PRIVATE qtng Catch2::Catch2WithMain)
    target_include_directories(qtng_test_quic PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}")
    set_target_properties(qtng_test_quic PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    catch_discover_tests(qtng_test_quic)

    # Optional picoquic interop (process-level). Requires an external picoquicdemo binary.
    # See 3rdparty/README.md / README for how to build picoquic into /tmp/picoquic-build.
    add_executable(qtng_test_quic_picoquic
        test_quic_picoquic.cpp
    )
    target_link_libraries(qtng_test_quic_picoquic PRIVATE qtng Catch2::Catch2WithMain)
    target_include_directories(qtng_test_quic_picoquic PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}")
    set_target_properties(qtng_test_quic_picoquic PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    catch_discover_tests(qtng_test_quic_picoquic)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/3rdparty/libutp/utp.h")
    include("${CMAKE_SOURCE_DIR}/3rdparty/libutp.cmake")
    add_executable(qtng_test_utp_libutp
        test_utp_libutp.cpp
    )
    target_link_libraries(qtng_test_utp_libutp PRIVATE qtng utp Catch2::Catch2WithMain)
    target_include_directories(qtng_test_utp_libutp PRIVATE "${CMAKE_SOURCE_DIR}/include" "${CMAKE_SOURCE_DIR}")
    set_target_properties(qtng_test_utp_libutp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    catch_discover_tests(qtng_test_utp_libutp)
    message(STATUS "3rdparty/libutp found: building uTP libutp interoperability tests")
else()
    message(STATUS "3rdparty/libutp not initialized: skipping uTP libutp interoperability tests")
endif()
