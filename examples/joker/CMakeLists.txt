cmake_minimum_required(VERSION 3.14...3.28 FATAL_ERROR)

project(jokerproxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JOKER_SERVER_SRC
    server_main.cpp
    joker_server.cpp
    joker_server.h
)

set(JOKER_CLIENT_SRC
    client_main.cpp
    joker_client.cpp
    joker_client.h
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../.." qtng)
else()
    add_subdirectory(qtng)
endif()

# MinGW XP toolchains: avoid Vista+ imports without patching LibreSSL sources.
# - pthread.h: prepend our include dir so LibreSSL TUs see XP pthread_once
# - getentropy: provide our object first so libcrypto's BCrypt getentropy is not pulled
# - inet_pton / GetTickCount64: local import thunks
set(JOKER_XP_SHIMS)
if(MINGW AND JOKER_MINGW_XP_COMPAT)
    enable_language(C)
    set(_JOKER_XP_COMPAT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/xp-compat")

    # Prefer our pthread.h over LibreSSL include/compat (no file copies into libressl/).
    foreach(_t crypto_obj ssl_obj crypto ssl)
        if(TARGET ${_t})
            target_include_directories(${_t} BEFORE PRIVATE "${_JOKER_XP_COMPAT_DIR}")
        endif()
    endforeach()

    # qtng/joker use XP WINNT; LibreSSL keeps its own 0x0600 (separate TUs, no -D clash).
    if(TARGET qtng)
        target_compile_definitions(qtng PRIVATE
            WINVER=${JOKER_XP_WINVER}
            _WIN32_WINNT=${JOKER_XP_WINNT})
    endif()

    add_library(joker_xp_compat OBJECT
        cmake/xp-compat/xp_api_shims.c
        cmake/xp-compat/getentropy_win.c)
    target_include_directories(joker_xp_compat PRIVATE "${_JOKER_XP_COMPAT_DIR}")
    target_compile_definitions(joker_xp_compat PRIVATE
        WINVER=${JOKER_XP_WINVER}
        _WIN32_WINNT=${JOKER_XP_WINNT})
    target_link_libraries(joker_xp_compat PUBLIC ws2_32 user32 advapi32)

    # OBJECT sources first so local getentropy / __imp_* thunks win over archive/DLL imports.
    set(JOKER_XP_SHIMS $<TARGET_OBJECTS:joker_xp_compat>)
endif()

add_executable(joker-server ${JOKER_SERVER_SRC})
target_link_libraries(joker-server PRIVATE ${JOKER_XP_SHIMS} qtng)
if(JOKER_XP_SHIMS)
    target_compile_definitions(joker-server PRIVATE
        WINVER=${JOKER_XP_WINVER}
        _WIN32_WINNT=${JOKER_XP_WINNT})
    target_link_options(joker-server PRIVATE "-Wl,--allow-multiple-definition")
endif()
set_target_properties(joker-server PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(joker-server PROPERTIES LINK_FLAGS_RELEASE -s)

add_executable(joker-client ${JOKER_CLIENT_SRC})
target_link_libraries(joker-client PRIVATE ${JOKER_XP_SHIMS} qtng)
if(JOKER_XP_SHIMS)
    target_compile_definitions(joker-client PRIVATE
        WINVER=${JOKER_XP_WINVER}
        _WIN32_WINNT=${JOKER_XP_WINNT})
    target_link_options(joker-client PRIVATE "-Wl,--allow-multiple-definition")
endif()
set_target_properties(joker-client PROPERTIES LINK_FLAGS_MINSIZERELEASE -s)
set_target_properties(joker-client PROPERTIES LINK_FLAGS_RELEASE -s)
