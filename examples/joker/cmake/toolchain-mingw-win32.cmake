# MinGW-w64 i686 toolchain targeting Windows XP (5.1).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(CMAKE_C_COMPILER i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER i686-w64-mingw32-windres)
set(CMAKE_ADDR2LINE i686-w64-mingw32-addr2line)
set(CMAKE_AR i686-w64-mingw32-ar)
set(CMAKE_NM i686-w64-mingw32-nm)
set(CMAKE_OBJCOPY i686-w64-mingw32-objcopy)
set(CMAKE_OBJDUMP i686-w64-mingw32-objdump)
set(CMAKE_RANLIB i686-w64-mingw32-ranlib)
set(CMAKE_STRIP i686-w64-mingw32-strip)

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Do NOT add_compile_definitions(_WIN32_WINNT=...) globally: LibreSSL also defines
# _WIN32_WINNT=0x0600 and a second -D triggers "redefined" warnings. Apply XP
# WINNT only to qtng/joker targets (see examples/joker/CMakeLists.txt).
set(JOKER_MINGW_XP_COMPAT ON CACHE BOOL "Link XP API shims for mingw" FORCE)
set(JOKER_XP_WINVER "0x0501" CACHE STRING "WINVER for qtng/joker XP builds" FORCE)
set(JOKER_XP_WINNT "0x0501" CACHE STRING "_WIN32_WINNT for qtng/joker XP builds" FORCE)

# -static pulls in libwinpthread.a (posix model) so no libwinpthread-1.dll at runtime.
# xp_api_shims.c supplies GetTickCount64/inet_pton import thunks missing on XP.
set(_XP_LINK_FLAGS
    "-static -Wl,--major-os-version,5 -Wl,--minor-os-version,1 -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1"
)

# FORCE: FLAGS_INIT only applies on first configure of a build tree.
set(CMAKE_EXE_LINKER_FLAGS "${_XP_LINK_FLAGS}" CACHE STRING "XP exe linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "${_XP_LINK_FLAGS}" CACHE STRING "XP shared linker flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "${_XP_LINK_FLAGS}" CACHE STRING "XP module linker flags" FORCE)
