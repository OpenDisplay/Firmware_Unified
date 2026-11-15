# CMake toolchain for Telink TC32 (tc32-elf-gcc 4.5.1, "Telink TC32 version 2.0 build").
#
# The toolchain is not on PATH and is not vendored: TC32_DIR points at the directory that
# holds bin/tc32-elf-gcc (build.sh exports it). Linking uses tc32-elf-ld directly, as the
# Telink SDK's own projects do: the SDK's boot.link and its library order are written for ld,
# and libgcc is pulled explicitly (see libc/od_rt.c for why that is not the default).
set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR tc32)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED TC32_DIR)
  set(TC32_DIR "$ENV{TC32_DIR}")
endif()
if(NOT EXISTS "${TC32_DIR}/bin/tc32-elf-gcc")
  message(FATAL_ERROR "tc32-elf-gcc not found: set TC32_DIR to the toolchain root (holding bin/tc32-elf-gcc)")
endif()

set(CMAKE_C_COMPILER   "${TC32_DIR}/bin/tc32-elf-gcc")
set(CMAKE_ASM_COMPILER "${TC32_DIR}/bin/tc32-elf-gcc")
set(CMAKE_AR           "${TC32_DIR}/bin/tc32-elf-ar")
set(CMAKE_OBJCOPY      "${TC32_DIR}/bin/tc32-elf-objcopy")
set(CMAKE_SIZE         "${TC32_DIR}/bin/tc32-elf-size")
set(TC32_LD            "${TC32_DIR}/bin/tc32-elf-ld")
set(TC32_LIBGCC_DIR    "${TC32_DIR}/lib/gcc/tc32-elf/4.5.1.tc32-elf-1.5")

set(CMAKE_C_COMPILER_WORKS TRUE)
set(CMAKE_ASM_COMPILER_WORKS TRUE)
set(CMAKE_C_STANDARD_COMPUTED_DEFAULT 99)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
