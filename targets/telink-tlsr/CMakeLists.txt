# OpenDisplay firmware for Telink TLSR825x tags (the chips ATC_BLE_OEPL runs on).
#
#   cmake -S targets/telink-tlsr -B build/telink -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=targets/telink-tlsr/cmake/tc32-toolchain.cmake
#   cmake --build build/telink
#
# build.sh does exactly that after tools/fetch_deps.sh has fetched the pinned SDK and toolchain.
#
# TWO COMPILE DOMAINS, and keeping them apart is the design. The prebuilt BLE library was built
# with -fpack-struct -fshort-enums, so every file that includes an SDK header must be too. shared/
# must not be: its wire structs assert their own sizes and its enums cross into od_hal_* by value.
# src/tlsr_port.h is the only thing both sides see, and it carries no struct and no enum.

cmake_minimum_required(VERSION 3.20)
project(opendisplay_tlsr C ASM)

get_filename_component(OD_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(TELINK_SDK_DIR "${OD_REPO_ROOT}/build/deps/telink-tlsr/tc_ble_single_sdk/tc_ble_single_sdk"
    CACHE PATH "Telink tc_ble_single_sdk root (the directory holding tl_common.h)")
if(NOT EXISTS "${TELINK_SDK_DIR}/tl_common.h")
  message(FATAL_ERROR "Telink SDK not found at ${TELINK_SDK_DIR}; run tools/fetch_deps.sh")
endif()

set(SRC "${CMAKE_CURRENT_LIST_DIR}/src")

# ---------------------------------------------------------------------------- shared/ ---
include(${OD_REPO_ROOT}/shared/sources.cmake)

# BG22's profile (the other no-RTOS, no-PIPE target), with two differences: the fleet-wide
# 4096-byte config cap (decision 12 -- there is RAM for it here), and the RX ring, which this
# target needs because the Telink ATT write callback cannot hold a frame back the way BGAPI can.
set(OD_PROFILE_TELINK
    OD_TXQ_SLOTS=3u
    OD_RXQ_SLOTS=8u
    OD_CAP_PIPE=0
    OD_CAP_PARTIAL=0
    OD_CAP_RXQ=1
    OD_CAP_DUAL_CS=0
    OD_CAP_LOG=0
    OD_CONFIG_WITH_TOUCH=0
    OD_CONFIG_WITH_BUZZER=0
    OD_CONFIG_WITH_WIFI=0
    OD_CONFIG_WITH_DATA_EXTENDED=1)   # identity strings (0x2C); 288 B, and the boot-screen header lines
set(OD_TELINK_DEFS ${OD_PROFILE_TELINK})
od_select_log_profile(info OD_TELINK_DEFS)

set(OD_TELINK_SHARED_SOURCES
    ${OD_SHARED_SOURCES_PURE}
    ${OD_SHARED_SOURCES_HAL_NVS}
    ${OD_SHARED_SOURCES_HAL_CRYPTO}
    ${OD_SHARED_SOURCES_HAL_RADIO}
    ${OD_SHARED_SOURCES_APP_SESSION}
    ${OD_SHARED_SOURCES_APP_RXQ}
    ${OD_SHARED_SOURCES_APP_INFLATE}
    ${OD_SHARED_SOURCES_APP_XFER}
    ${OD_SHARED_SOURCES_APP_NFC}
    ${OD_SHARED_SOURCES_APP_BOOT}
    ${OD_SHARED_SOURCES_APP_LED}
    ${OD_REPO_ROOT}/third_party/qrcode/qrcode.c)

add_library(od_side OBJECT
    ${OD_TELINK_SHARED_SOURCES}
    ${SRC}/od_aes_modes.c
    ${SRC}/od_cmd_tlsr.c
    ${SRC}/od_hal_crypto.c
    ${SRC}/od_hal_tlsr.c
    ${SRC}/od_tlsr_app.c
    ${SRC}/od_tlsr_rt.c
    ${SRC}/od_tlsr_fmt.c
    ${SRC}/od_xfer_tlsr.c
    ${SRC}/od_led_tlsr.c
    # Panel drivers imported from Firmware_NRF52 (EPD/, 71b870c1); epd_io.c is their I/O here.
    ${SRC}/epd/UC81xx.c
    ${SRC}/epd/SSD16xx.c
    ${SRC}/epd/TI97xx.c
    ${SRC}/epd/epd_models.c
    ${SRC}/epd/epd_io.c)
target_include_directories(od_side PRIVATE ${OD_SHARED_INCLUDE_DIRS} ${SRC} ${SRC}/epd)
# The boot logo is only drawn on 400x300 and larger; one size keeps ~70 KB of bitmap out of flash.
target_compile_definitions(od_side PRIVATE ${OD_TELINK_DEFS} OD_BOOT_LOGO_SIZES=1)
# -O2, never -Os: tc32 at -Os lays switch tables out as byte offsets but dispatches them as 32-bit
# addresses, so the first switch taken jumps into garbage (od_frame_policy did, on hardware).
# The SDK is built at -O2 for the same reason; tools/check_jump_tables.py guards it.
target_compile_options(od_side PRIVATE
    -std=gnu99 -O2 -ffunction-sections -fdata-sections -Wall
    -include ${SRC}/tc32_compat.h)

# ----------------------------------------------------------------------------- SDK side ---
file(GLOB TELINK_SDK_C
    ${TELINK_SDK_DIR}/drivers/B85/*.c
    ${TELINK_SDK_DIR}/drivers/B85/driver_ext/*.c
    ${TELINK_SDK_DIR}/drivers/B85/flash/*.c
    ${TELINK_SDK_DIR}/common/*.c
    ${TELINK_SDK_DIR}/vendor/common/*.c
    ${TELINK_SDK_DIR}/application/print/*.c)

add_library(sdk_side OBJECT
    ${TELINK_SDK_C}
    ${TELINK_SDK_DIR}/boot/B85/cstartup_825x.S
    ${TELINK_SDK_DIR}/common/div_mod.S
    ${SRC}/main.c
    ${SRC}/app.c
    ${SRC}/app_att.c
    ${SRC}/tlsr_port.c)
target_include_directories(sdk_side PRIVATE
    ${TELINK_SDK_DIR} ${TELINK_SDK_DIR}/drivers/B85 ${TELINK_SDK_DIR}/vendor/common
    ${TELINK_SDK_DIR}/common ${SRC})
target_compile_definitions(sdk_side PRIVATE CHIP_TYPE=CHIP_TYPE_825x MCU_STARTUP_8258=1)
target_compile_options(sdk_side PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99 -O2 -ffunction-sections -fdata-sections -Wall
                            -fpack-struct -fshort-enums -finline-small-functions
                            -fshort-wchar -fms-extensions -include ${SRC}/app_config.h>)

# ---------------------------------------------------------------------------- link ---
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
                OUTPUT_VARIABLE TC32_LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(TC32_LIBGCC_DIR "${TC32_LIBGCC}" DIRECTORY)
set(CMAKE_LINKER "${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-ld")
set(CMAKE_C_LINK_EXECUTABLE "<CMAKE_LINKER> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")

add_executable(opendisplay_tlsr.elf $<TARGET_OBJECTS:sdk_side> $<TARGET_OBJECTS:od_side>)
set_target_properties(opendisplay_tlsr.elf PROPERTIES LINKER_LANGUAGE C)
target_link_options(opendisplay_tlsr.elf PRIVATE
    --gc-sections
    -T ${TELINK_SDK_DIR}/project/tlsr_tc32/B85/boot.link
    -Map ${CMAKE_BINARY_DIR}/opendisplay_tlsr.map)
target_link_libraries(opendisplay_tlsr.elf PRIVATE
    -L${TELINK_SDK_DIR}/proj_lib -llt_825x -llt_general_stack
    -L${TC32_LIBGCC_DIR} -lgcc)

# Telink's check_fw step: CRC32 tail + length field. Updaters (Telink OTA, ATC_BLE_OEPL) refuse
# an image without it. finish_image.py is a portable reimplementation, verified against ATC's image.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(TARGET opendisplay_tlsr.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary opendisplay_tlsr.elf opendisplay_tlsr.bin
    COMMAND ${CMAKE_SIZE} opendisplay_tlsr.elf
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/finish_image.py
            ${CMAKE_BINARY_DIR}/opendisplay_tlsr.bin
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/check_jump_tables.py
            ${TC32_TOOLCHAIN_DIR}/bin/tc32-elf-objdump opendisplay_tlsr.elf opendisplay_tlsr.bin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    BYPRODUCTS ${CMAKE_BINARY_DIR}/opendisplay_tlsr.bin)
