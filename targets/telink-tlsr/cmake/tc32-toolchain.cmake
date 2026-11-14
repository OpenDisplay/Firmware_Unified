# CMake toolchain file for Telink's tc32-elf-gcc (GCC 4.5.1, TC32 ISA).
#
# TC32_TOOLCHAIN_DIR is the toolchain root (the directory holding bin/tc32-elf-gcc);
# tools/fetch_deps.sh puts it in <repo>/build/deps/telink-tlsr/tc32. The compiler cannot link a
# hosted test program, so CMake's compiler check builds a static library instead.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR tc32)

if(NOT TC32_TOOLCHAIN_DIR)
  set(TC32_TOOLCHAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../build/deps/telink-tlsr/tc32")
endif()
get_filename_component(TC32_TOOLCHAIN_DIR "${TC32_TOOLCHAIN_DIR}" ABSOLUTE)
set(TC32_TOOLCHAIN_DIR "${TC32_TOOLCHAIN_DIR}" CACHE PATH "tc32 toolchain root")

set(CMAKE_C_COMPILER   "${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-gcc")
set(CMAKE_ASM_COMPILER "${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-gcc")
set(CMAKE_AR           "${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-ranlib" CACHE FILEPATH "")
set(CMAKE_OBJCOPY      "${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-objcopy" CACHE FILEPATH "")
set(CMAKE_SIZE         "${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-size" CACHE FILEPATH "")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
