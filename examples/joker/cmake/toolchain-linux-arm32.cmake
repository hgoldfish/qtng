# musl.cc cross toolchain for fully static 32-bit ARM (ARMv7 hard-float) Linux binaries.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(JOKER_MUSL_CROSS_ROOT "/opt/cross/arm-linux-musleabihf-cross"
    CACHE PATH "musl.cc cross toolchain prefix")
set(JOKER_MUSL_TRIPLE "arm-linux-musleabihf" CACHE STRING "target triple")

# The toolchain defaults to armv5te+fp with -mfpu=auto, which cannot assemble
# qtng's VFP context switch (vstmia d8-d15). Use the Debian armhf baseline.
set(JOKER_ARM_ARCH_FLAGS "-march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT "${JOKER_ARM_ARCH_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${JOKER_ARM_ARCH_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${JOKER_ARM_ARCH_FLAGS}")

set(CMAKE_C_COMPILER "${JOKER_MUSL_CROSS_ROOT}/bin/${JOKER_MUSL_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER "${JOKER_MUSL_CROSS_ROOT}/bin/${JOKER_MUSL_TRIPLE}-g++")
set(CMAKE_AR "${JOKER_MUSL_CROSS_ROOT}/bin/${JOKER_MUSL_TRIPLE}-ar")
set(CMAKE_RANLIB "${JOKER_MUSL_CROSS_ROOT}/bin/${JOKER_MUSL_TRIPLE}-ranlib")
set(CMAKE_STRIP "${JOKER_MUSL_CROSS_ROOT}/bin/${JOKER_MUSL_TRIPLE}-strip")

# Keep every find_* inside the musl sysroot: the host has no target libraries.
set(CMAKE_FIND_ROOT_PATH "${JOKER_MUSL_CROSS_ROOT}/${JOKER_MUSL_TRIPLE}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# FORCE: FLAGS_INIT only applies on first configure of a build tree.
# -no-pie: Debian's gcc defaults to PIE, but these artifacts should be plain
# static executables (like the other joker builds), not static-pie.
set(CMAKE_EXE_LINKER_FLAGS "-static -no-pie" CACHE STRING "static musl exe linker flags" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS "-static" CACHE STRING "static musl shared linker flags" FORCE)
set(CMAKE_MODULE_LINKER_FLAGS "-static" CACHE STRING "static musl module linker flags" FORCE)
