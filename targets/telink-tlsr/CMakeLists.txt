# Telink TLSR825x (B85) e-paper tags: TLSR8359 and relatives, the parts the community OpenEPaperLink BLE
# firmware runs on. Built by ./build.sh, which sets TC32_DIR and TELINK_SDK_DIR.
#
# The Telink BLE SDK is referenced by path and never vendored: TELINK_SDK_DIR is the SDK root
# (the directory holding drivers/, stack/ and proj_lib/), tag V3.4.3.0_Patch_0002. Its own
# vendor/ble_sample/app_config.h is shadowed by sdk_overlay/ so the SDK tree stays unedited.
cmake_minimum_required(VERSION 3.16)

set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/cmake/tc32-toolchain.cmake" CACHE FILEPATH "")
project(opendisplay_tlsr C ASM)

if(NOT DEFINED TELINK_SDK_DIR)
  set(TELINK_SDK_DIR "$ENV{TELINK_SDK_DIR}")
endif()
if(NOT EXISTS "${TELINK_SDK_DIR}/proj_lib/liblt_825x.a")
  message(FATAL_ERROR "TELINK_SDK_DIR must point at the tc_ble_single_sdk root (holding proj_lib/liblt_825x.a)")
endif()
set(SDK "${TELINK_SDK_DIR}")

set(OD_SHARED_DIR "${CMAKE_CURRENT_LIST_DIR}/../../shared")
include("${OD_SHARED_DIR}/sources.cmake")

# ---- firmware version: one string, tools/release/version.sh decides it ----
if(NOT DEFINED OD_FW_VERSION OR "${OD_FW_VERSION}" STREQUAL "")
  file(STRINGS "${OD_SHARED_DIR}/core/od_fw_version.h" _ver REGEX "OD_FW_VERSION_DEFAULT")
  string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _m "${_ver}")
  set(OD_FW_VERSION "${CMAKE_MATCH_1}")
endif()
if(NOT OD_FW_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
  message(FATAL_ERROR "OD_FW_VERSION '${OD_FW_VERSION}' is not X.Y.Z")
endif()
set(OD_FW_MAJOR ${CMAKE_MATCH_1})
set(OD_FW_MINOR ${CMAKE_MATCH_2})
set(OD_FW_PATCH ${CMAKE_MATCH_3})
if(NOT DEFINED OD_BUILD_ID)
  set(OD_BUILD_ID "tlsr-dev")
endif()

# Fault injection for the deep-retention fallback (src/od_tlsr_display.c, PIN_FAULT_ARM).
# Opt-in, never part of a release.
option(OD_TLSR_FAULT_TEST "arm-able hang in the retention wake path" OFF)

# Raw SPI (CMD_SPI_RAW) and its GPIO sub-op, for characterising a panel and measuring idle
# power over the air. On an unclaimed tag they drive any pin but SWS without authentication, so
# they are a bring-up build's, not a release's. The fault-test build arms through the GPIO
# sub-op and turns them on.
option(OD_TLSR_BRINGUP "raw SPI and the GPIO sub-op (CMD_SPI_RAW)" OFF)
if(OD_TLSR_FAULT_TEST)
  set(OD_TLSR_BRINGUP ON)
endif()
if(OD_TLSR_BRINGUP)
  set(OD_TLSR_SPI_RAW 1)
else()
  set(OD_TLSR_SPI_RAW 0)
endif()

# ---- the shared profile for this target ----
# Kept here, not in shared/profiles.cmake, while the target is provisional (FOLLOWUPS: move it
# there as OD_PROFILE_TELINK when the target is accepted, so one file defines every profile).
# The cap is the fleet's 4096 (CLAUDE.md decision 12): this part has 64 KB of SRAM, so it does
# not need BG22's exception.
set(OD_PROFILE_TELINK
    OD_CONFIG_MAX_SIZE=4096u
    # PIPE window 6: the reorder ring is (W+1) x 241 B and both rings hold W+2 frames, about
    # 5.6 KB together. The LL RX FIFO (od_tlsr_fifo.c) caps the air rate well below what a wider
    # window would use.
    OD_CAP_PIPE=1
    OD_PIPE_MAX_W=6u
    OD_TXQ_SLOTS=8u
    OD_RXQ_SLOTS=8u
    OD_CAP_PARTIAL=0
    OD_CAP_RXQ=0
    OD_CAP_DUAL_CS=0
    OD_CAP_LOG=0
    OD_CAP_NFC=0
    OD_CAP_IMGSTORE=0
    OD_CAP_ACTION=0
    OD_CAP_DRIVE_WRITE=0
    OD_CAP_SPI_RAW=${OD_TLSR_SPI_RAW}
    OD_CAP_SPI_RAW_GPIO=${OD_TLSR_SPI_RAW}
    OD_CAP_READ_IDENT=1
    OD_CONFIG_WITH_TOUCH=0
    OD_CONFIG_WITH_BUZZER=0
    OD_CONFIG_WITH_WIFI=0
    OD_CONFIG_WITH_NFC=0
    OD_CONFIG_WITH_FINDMY=0
    OD_CONFIG_WITH_DATA_EXTENDED=0
    OD_EPD_WITH_WINDOW=0
    OD_EPD_LUT_CACHE=0
    # The scale-2 and -3 boot logos are chosen only under 30 % of a 500+ px logical width;
    # these tags' glass is 200 px, so only scale 1 can ever be drawn (17.6 KB of flash saved).
    OD_BOOT_LOGO_SIZES=1)

# READ_IDENT fw_target 0x06 is provisional (docs/PROTOCOL_CHANGES_3.0.md). The canonical
# OD_IDENT_TARGET_TELINK belongs in ../opendisplay-protocol; until it is there the number is
# named here only.
set(OD_IDENT_TARGET_TELINK 0x06)

# ---- SDK sources (everything the ble_sample project compiles, minus its vendor/ app) ----
file(GLOB_RECURSE SDK_SOURCES
     "${SDK}/algorithm/*.c" "${SDK}/application/*.c" "${SDK}/common/*.c"
     "${SDK}/drivers/B85/*.c" "${SDK}/stack/*.c" "${SDK}/vendor/common/*.c"
     "${SDK}/common/*.S" "${SDK}/boot/B85/*.S")

set(SDK_DEFS MCU_STARTUP_8258 __PROJECT_8258_BLE_SAMPLE__=1 CHIP_TYPE=CHIP_TYPE_825x)
set(SDK_INCLUDES
    "${CMAKE_CURRENT_LIST_DIR}/sdk_overlay"   # ahead of the SDK: shadows vendor/ble_sample/app_config.h
    "${SDK}" "${SDK}/vendor/common" "${SDK}/common" "${SDK}/drivers/B85")
# -O2, not -Os: see libc/od_rt.c. -fshort-wchar and -fms-extensions are the SDK's own flags.
set(TC32_CFLAGS -O2 -std=gnu99 -fshort-wchar -fms-extensions -finline-small-functions
    -ffunction-sections -fdata-sections)

add_library(tlsr_sdk OBJECT ${SDK_SOURCES})
target_compile_definitions(tlsr_sdk PRIVATE ${SDK_DEFS})
target_include_directories(tlsr_sdk PRIVATE ${SDK_INCLUDES})
# -fpack-struct: the SDK's own projects build with it, and liblt_825x.a was (src/od_tlsr_sdk.h).
target_compile_options(tlsr_sdk PRIVATE ${TC32_CFLAGS} -fpack-struct -w)

# ---- OpenDisplay: shared tiers + this target ----
set(OD_TLSR_SHARED_SOURCES
    ${OD_SHARED_SOURCES_PURE}
    ${OD_SHARED_SOURCES_HAL_NVS}
    ${OD_SHARED_SOURCES_HAL_CRYPTO}
    ${OD_SHARED_SOURCES_HAL_RADIO}
    ${OD_SHARED_SOURCES_APP_SESSION}
    ${OD_SHARED_SOURCES_APP_INFLATE}
    ${OD_SHARED_SOURCES_APP_XFER}
    ${OD_SHARED_SOURCES_APP_NFC}
    ${OD_SHARED_SOURCES_APP_IDENT}
    ${OD_SHARED_SOURCES_APP_EPD}
    ${OD_SHARED_SOURCES_APP_DRIVE}
    ${OD_SHARED_SOURCES_APP_BOOT}
    ${OD_SHARED_SOURCES_APP_DISPLAY}
    ${OD_SHARED_SOURCES_APP_SPI_RAW}
    ${OD_SHARED_SOURCES_HAL_LOG})

set(OD_TLSR_TARGET_SOURCES
    src/main.c
    src/od_tlsr_ble.c
    src/od_tlsr_cmd.c
    src/od_tlsr_seams.c
    src/od_tlsr_display.c
    src/od_tlsr_hal.c
    src/od_tlsr_flash.c
    src/od_tlsr_store.c
    src/od_tlsr_crypto.c
    src/od_tlsr_crypto_hw.c
    src/od_tlsr_ota.c
    src/od_tlsr_install.c
    src/od_tlsr_fifo.c
    libc/od_libc.c
    libc/od_rt.c
    "${CMAKE_CURRENT_LIST_DIR}/../../third_party/qrcode/qrcode.c")

set(OD_DEFS ${OD_PROFILE_TELINK}
    OD_IDENT_TARGET=${OD_IDENT_TARGET_TELINK}
    OD_FW_MAJOR=${OD_FW_MAJOR} OD_FW_MINOR=${OD_FW_MINOR} OD_FW_PATCH=${OD_FW_PATCH}
    OD_BUILD_ID="${OD_BUILD_ID}")
set(OD_FORCE_INCLUDE -include "${CMAKE_CURRENT_LIST_DIR}/libc/include/od_tlsr_compat.h")

add_library(od_shared OBJECT ${OD_TLSR_SHARED_SOURCES})
target_compile_definitions(od_shared PRIVATE ${OD_DEFS})
target_include_directories(od_shared PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libc/include" ${OD_SHARED_INCLUDE_DIRS})
target_compile_options(od_shared PRIVATE ${TC32_CFLAGS} ${OD_FORCE_INCLUDE} -Wall -Wno-unused-function)

add_library(od_tlsr OBJECT ${OD_TLSR_TARGET_SOURCES})
target_compile_definitions(od_tlsr PRIVATE ${OD_DEFS} ${SDK_DEFS})
target_include_directories(od_tlsr PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}/libc/include"
    ${OD_SHARED_INCLUDE_DIRS} ${SDK_INCLUDES})
target_compile_options(od_tlsr PRIVATE ${TC32_CFLAGS} ${OD_FORCE_INCLUDE} -Wall)

# ---- link with tc32-elf-ld, as the SDK's projects do ----
# od_rt.o precedes -lgcc (libc/od_rt.c says why); the SDK's two binary libraries follow the
# objects that reference them.
# Each image names its script in LINK_FLAGS. The OpenDisplay image keeps every static in the
# deep-retention bank (tools/retention_link.py derives that script from the SDK's at configure
# time); the probes use the SDK's script unchanged.
set(SDK_BOOT_LINK "${SDK}/project/tlsr_tc32/B85/boot.link")
set(OD_RETENTION_LINK "${CMAKE_BINARY_DIR}/boot_retention.link")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_LIST_DIR}/tools/retention_link.py"
            "${SDK_BOOT_LINK}" "${OD_RETENTION_LINK}"
    RESULT_VARIABLE retention_link_rc)
if(NOT retention_link_rc EQUAL 0)
  message(FATAL_ERROR "tools/retention_link.py failed on ${SDK_BOOT_LINK}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SDK_BOOT_LINK}" "${CMAKE_CURRENT_LIST_DIR}/tools/retention_link.py")
set(CMAKE_C_LINK_EXECUTABLE
    "${TC32_LD} --gc-sections -L${SDK}/proj_lib <LINK_FLAGS> -Map <TARGET>.map -o <TARGET> <OBJECTS> -llt_825x -llt_general_stack -L${TC32_LIBGCC_DIR} -lgcc")

if(OD_TLSR_FAULT_TEST)
  target_compile_definitions(od_tlsr PRIVATE OD_TLSR_FAULT_TEST=1)
endif()

add_executable(opendisplay_tlsr.elf
    $<TARGET_OBJECTS:od_tlsr> $<TARGET_OBJECTS:od_shared> $<TARGET_OBJECTS:tlsr_sdk>)
set_target_properties(opendisplay_tlsr.elf PROPERTIES LINKER_LANGUAGE C
    LINK_FLAGS "-T ${OD_RETENTION_LINK}")

# ---- the bring-up probe (src/probe/od_tlsr_probe.c): link layer + updater, no protocol stack.
# The CRC comes from od_config_store.c, the one CRC-32 implementation; --gc-sections drops the
# rest of that file, so nothing else of shared/ is linked.
add_library(od_tlsr_probe OBJECT
    src/probe/od_tlsr_probe.c src/od_tlsr_fifo.c src/od_tlsr_ota.c src/od_tlsr_install.c src/od_tlsr_flash.c
    "${OD_SHARED_DIR}/core/od_config_store.c" libc/od_libc.c libc/od_rt.c)
target_compile_definitions(od_tlsr_probe PRIVATE ${OD_DEFS} ${SDK_DEFS})
target_include_directories(od_tlsr_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}/libc/include"
    ${OD_SHARED_INCLUDE_DIRS} ${SDK_INCLUDES})
target_compile_options(od_tlsr_probe PRIVATE ${TC32_CFLAGS} ${OD_FORCE_INCLUDE} -Wall)
add_executable(opendisplay_tlsr_probe.elf $<TARGET_OBJECTS:od_tlsr_probe> $<TARGET_OBJECTS:tlsr_sdk>)
set_target_properties(opendisplay_tlsr_probe.elf PROPERTIES LINKER_LANGUAGE C
    LINK_FLAGS "-T ${SDK_BOOT_LINK}")

# Power-floor test image (src/probe/od_tlsr_probe.c, OD_TLSR_PMTEST): the probe cycling through
# suspend and deep-sleep phases for a current meter. Opt-in, never part of a release.
option(OD_TLSR_PMTEST "also build opendisplay_tlsr_pmtest.bin" OFF)
set(OD_TLSR_IMAGES opendisplay_tlsr opendisplay_tlsr_probe)
if(OD_TLSR_PMTEST)
  add_library(od_tlsr_pmtest OBJECT
      src/probe/od_tlsr_probe.c src/od_tlsr_fifo.c src/od_tlsr_ota.c src/od_tlsr_install.c src/od_tlsr_flash.c
      "${OD_SHARED_DIR}/core/od_config_store.c" libc/od_libc.c libc/od_rt.c)
  target_compile_definitions(od_tlsr_pmtest PRIVATE ${OD_DEFS} ${SDK_DEFS} OD_TLSR_PMTEST=1)
  target_include_directories(od_tlsr_pmtest PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}/libc/include"
      ${OD_SHARED_INCLUDE_DIRS} ${SDK_INCLUDES})
  target_compile_options(od_tlsr_pmtest PRIVATE ${TC32_CFLAGS} ${OD_FORCE_INCLUDE} -Wall)
  add_executable(opendisplay_tlsr_pmtest.elf $<TARGET_OBJECTS:od_tlsr_pmtest> $<TARGET_OBJECTS:tlsr_sdk>)
  set_target_properties(opendisplay_tlsr_pmtest.elf PROPERTIES LINKER_LANGUAGE C
      LINK_FLAGS "-T ${SDK_BOOT_LINK}")
  list(APPEND OD_TLSR_IMAGES opendisplay_tlsr_pmtest)
endif()

foreach(img ${OD_TLSR_IMAGES})
  add_custom_command(TARGET ${img}.elf POST_BUILD
      COMMAND ${CMAKE_OBJCOPY} -O binary ${img}.elf ${img}.raw
      COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_LIST_DIR}/tools/tlsr_image.py"
              seal ${img}.raw ${img}.bin
      COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_LIST_DIR}/tools/check_ram_install.py"
              --objdump "${TC32_DIR}/bin/tc32-elf-objdump" --elf ${img}.elf
              --map ${img}.elf.map
      COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_CURRENT_LIST_DIR}/tools/check_ll_fifos.py"
              --nm "${TC32_DIR}/bin/tc32-elf-nm" --elf ${img}.elf
      COMMAND ${CMAKE_SIZE} ${img}.elf
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      VERBATIM)
endforeach()
