# MinGW-w64 x86_64 toolchain targeting Windows XP x64 / Server 2003 (5.2).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_ADDR2LINE x86_64-w64-mingw32-addr2line)
set(CMAKE_AR x86_64-w64-mingw32-ar)
set(CMAKE_NM x86_64-w64-mingw32-nm)
set(CMAKE_OBJCOPY x86_64-w64-mingw32-objcopy)
set(CMAKE_OBJDUMP x86_64-w64-mingw32-objdump)
set(CMAKE_RANLIB x86_64-w64-mingw32-ranlib)
set(CMAKE_STRIP x86_64-w64-mingw32-strip)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

add_compile_definitions(WINVER=0x0502 _WIN32_WINNT=0x0502)

# -static pulls in libwinpthread.a (posix model) so no libwinpthread-1.dll at runtime.
set(_XP_LINK_FLAGS
    "-static -Wl,--major-os-version,5 -Wl,--minor-os-version,2 -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,2"
)

# FORCE: FLAGS_INIT only applies on first configure of a build tree.
set(CMAKE_EXE_LINKER_FLAGS "${_XP_LINK_FLAGS}" CACHE STRING "XP exe linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "${_XP_LINK_FLAGS}" CACHE STRING "XP shared linker flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "${_XP_LINK_FLAGS}" CACHE STRING "XP module linker flags" FORCE)
