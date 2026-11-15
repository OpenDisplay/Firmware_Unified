# ESP-IDF project for the OpenDisplay ESP32 targets.
#
# Phase B of the import (docs/MIGRATION.md § "The ESP32 import is different"). The goal of this
# phase is to LINK AND BOOT ON HARDWARE EARLY, so that every later step -- each subsystem
# promoted into shared/core -- is bisectable against a known-good baseline. It is not to be
# clean; cleanliness is phase C's job, enforced by tools/check.sh's arduino-free check.
#
#   ./build.sh <board>          e.g. ./build.sh s3-n16r8
#
# Boards are CMake fragments under boards/, not PlatformIO envs. The knob-by-knob translation
# from the source repo's platformio.ini is in ../../docs/TOOLCHAINS.md.

cmake_minimum_required(VERSION 3.16)

# OD_BOARD selects the board fragment. build.sh sets it; a bare `idf.py build` needs
# -DOD_BOARD=<board>, and failing loudly beats silently building a default nobody asked for.
if(NOT DEFINED OD_BOARD)
    message(FATAL_ERROR
        "OD_BOARD is not set. Use ./build.sh <board>, or pass -DOD_BOARD=<board>.\n"
        "Available: see boards/*.cmake")
endif()

set(OD_BOARD_FILE "${CMAKE_CURRENT_LIST_DIR}/boards/${OD_BOARD}.cmake")
if(NOT EXISTS "${OD_BOARD_FILE}")
    message(FATAL_ERROR "Unknown board '${OD_BOARD}' -- no ${OD_BOARD_FILE}")
endif()

# The fragment sets OD_IDF_TARGET, OD_BOARD_DEFINES, OD_PARTITION_CSV and any
# OD_SDKCONFIG_FRAGMENTS before the project() call, because IDF needs the target early.
include("${OD_BOARD_FILE}")

set(IDF_TARGET ${OD_IDF_TARGET})

set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
foreach(frag ${OD_SDKCONFIG_FRAGMENTS})
    set(SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS};${CMAKE_CURRENT_LIST_DIR}/${frag}")
endforeach()

# OD_PARTITION_CSV vs CONFIG_PARTITION_TABLE_CUSTOM_FILENAME: two places naming one layout.
#
# The partition table is really selected by the CONFIG_ line in the board's sdkconfig
# fragment; OD_PARTITION_CSV is documentation. That made it a trap -- set one, forget the
# other, and you silently build the wrong layout with nothing to notice. Rather than delete
# the variable (it is the only place a reader sees the layout next to the board's other
# facts), make disagreement a configure-time error.
if(DEFINED OD_PARTITION_CSV)
    if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/${OD_PARTITION_CSV}")
        message(FATAL_ERROR
            "Board '${OD_BOARD}' sets OD_PARTITION_CSV=${OD_PARTITION_CSV}, which does not exist.")
    endif()
    set(_od_csv_seen FALSE)
    foreach(frag ${OD_SDKCONFIG_FRAGMENTS})
        file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/${frag}" _od_lines
             REGEX "^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=")
        foreach(_line ${_od_lines})
            string(REGEX REPLACE "^CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"?([^\"]*)\"?$"
                   "\\1" _od_csv "${_line}")
            set(_od_csv_seen TRUE)
            if(NOT _od_csv STREQUAL OD_PARTITION_CSV)
                message(FATAL_ERROR
                    "Board '${OD_BOARD}' disagrees about its partition table:\n"
                    "  OD_PARTITION_CSV                     = ${OD_PARTITION_CSV}\n"
                    "  CONFIG_PARTITION_TABLE_CUSTOM_FILENAME = ${_od_csv}  (in ${frag})\n"
                    "The CONFIG_ line is what actually takes effect. Make them match.")
            endif()
        endforeach()
    endforeach()
    if(NOT _od_csv_seen)
        message(FATAL_ERROR
            "Board '${OD_BOARD}' sets OD_PARTITION_CSV but no sdkconfig fragment sets "
            "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME, so the layout would silently be IDF's "
            "default rather than ${OD_PARTITION_CSV}.")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "3.0.0")
endif()
project(opendisplay)

# ---------------------------------------------------------------------------------------
# The merged single-file image, built automatically with everything else.
# ---------------------------------------------------------------------------------------
# One file, flashed at offset 0, containing bootloader + partition table + OTA data + app.
# Produced on EVERY build rather than by a separate `idf.py merge-bin` step, because the
# four-image form is the one people get wrong: the offsets differ per chip (the classic
# ESP32 puts its bootloader at 0x1000, every other variant here at 0x0), and a merged image
# that is only generated on request is reliably stale by the time someone reaches for it --
# which is exactly what happened to the old dist/ image this replaces.
#
# `@flash_args` rather than a hand-written offset list, deliberately. That file is IDF's own
# record of what this board flashes where; spelling the offsets out here would be a second
# copy of the layout that could disagree with the partition table, and the 0x1000/0x0
# bootloader split makes that a live hazard rather than a theoretical one.
#
# The output stays in the build directory. Delivery to release/ is build.sh's job -- keeping
# the two separate means a plain `idf.py build` never writes outside its own build tree.
set(OD_MERGED_BIN "${CMAKE_BINARY_DIR}/opendisplay-${OD_BOARD}-merged.bin")

add_custom_target(od_merged_bin ALL
    BYPRODUCTS "${OD_MERGED_BIN}"
    COMMAND ${CMAKE_COMMAND} -E env python -m esptool
            --chip ${OD_IDF_TARGET} merge_bin -o "${OD_MERGED_BIN}" "@flash_args"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "od: merging bootloader + partition table + app -> opendisplay-${OD_BOARD}-merged.bin"
    VERBATIM)

# Every input the merge consumes. add_dependencies, not the DEPENDS argument above: for
# add_custom_target, DEPENDS names FILES, and three of these four are produced in other
# directory scopes where this one cannot see them as files. Naming the targets is what
# actually orders the build -- without it the merge races its own inputs and intermittently
# reads a partly-written or absent bootloader.
add_dependencies(od_merged_bin
    gen_project_binary      # the app .bin (not `app`, which is the phony umbrella)
    bootloader
    partition_table_bin
    blank_ota_data)

# ---------------------------------------------------------------------------------------
# Per-region parts + esp-web-tools manifest, and the app-only OTA image.
# ---------------------------------------------------------------------------------------
# The merged image above is for a bench flash to a blank part: it spans 0x0 to the end of
# the app, so it also overwrites NVS (0x9000-0xDFFF) with 0xFF and loses the config. The web
# flasher writes these parts at their own offsets instead and leaves NVS alone. The app part
# doubles as the BLE (SMP) OTA image. tools/web_flash_parts.py has the details.
idf_build_get_property(od_python PYTHON)
add_custom_target(od_web_parts ALL
    COMMAND ${od_python} ${CMAKE_CURRENT_LIST_DIR}/tools/web_flash_parts.py
            --build-dir "${CMAKE_BINARY_DIR}" --board "${OD_BOARD}"
            --chip "${OD_IDF_TARGET}" --version "${PROJECT_VER}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "od: web-flasher parts + manifest -> web/opendisplay-${OD_BOARD}-*"
    VERBATIM)
add_dependencies(od_web_parts
    gen_project_binary
    bootloader
    partition_table_bin
    blank_ota_data)
