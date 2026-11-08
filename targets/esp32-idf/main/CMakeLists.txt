# The application component.
#
# The imported sources live in ../src rather than in this directory, deliberately: phase A put
# them where the source repo had them so the import diff is a pure copy and blame stays
# readable. Registering them from here costs one relative path and keeps that property.

set(OD_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/../src)

# shared/ -- ONE source list, four consumers (shared/sources.cmake). This target is the first
# to consume it; until 2026-08-05 it took only shared/protocol's headers, so a file added to
# shared/ compiled on the host and nowhere else. That is exactly the gap the single list
# exists to close: adding a shared source that no target compiles must be a build error here,
# not an omission nobody notices.
#
# Sets OD_SHARED_SOURCES and OD_SHARED_INCLUDE_DIRS. Do not glob shared/ from this file.
include(${CMAKE_CURRENT_LIST_DIR}/../../../shared/sources.cmake)

file(GLOB OD_APP_SOURCES
    ${OD_SRC_DIR}/*.cpp
    ${OD_SRC_DIR}/*.c
)

list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/qrcode/qrcode.c)

# The FastEPD vendor adapter's storage, and millis() for FastEPD's call sites. PERMANENT --
# see ../vendor/fastepd/SPI.h and the scoping block further down.
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../vendor/fastepd/fastepd_adapter.cpp)

# Target HAL implementations. od_hal_nvs is the first: config storage moved off LittleFS onto
# NVS (2026-07-25), written to the shared/hal signature so promotion is a repoint.
# od_hal_log is the second (2026-08-04, phase C): it owns the log PORT so od_log.h could stop
# including <Arduino.h>. Its backend is a compile-time choice on OPENDISPLAY_LOG_UART.
# od_hal_gpio and od_hal_time are the third (2026-08-04, phase C step 3): the pin/clock seam
# the power-latch, buzzer and button files share. Signatures are SHARED_API_DESIGN.md's.
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_nvs.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_nvs_secure.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_log.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_gpio.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_time.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_i2c.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_adc.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_panel.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_wdt.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_crypto.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_crypto_random.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_radio.cpp
                           ${CMAKE_CURRENT_LIST_DIR}/../panel/od_panel_bbep.cpp
                           ${CMAKE_CURRENT_LIST_DIR}/../panel/od_panel_fastepd.cpp
                           ${CMAKE_CURRENT_LIST_DIR}/../ble/od_ble_nimble.cpp)

# Shared sources: the whole aggregate, because this target implements every HAL tier.
# od_hal_adv_{program,start,stop} are at the bottom of ble/od_ble_nimble.cpp; od_hal_wdt is
# hal/od_hal_wdt.c, listed above. Taking the aggregate rather than naming tiers is what makes a
# new shared source reach this build without an edit here.
list(APPEND OD_APP_SOURCES ${OD_SHARED_SOURCES})

# ble/od_ble_rx.cpp is GONE, not forgotten. It held od_ble_on_write(): the bounds check,
# the RX hex log and the enqueue. All three now live in src/command_queue.cpp's
# bleRxQueuePush(), which both targets share -- keeping a second copy here is what let the
# two transports report the same malformed frame differently.

# ---------------------------------------------------------------------------------------
# Panel libraries, vendored -- see ../../../third_party/NOTICE.md for origin and licence.
# ---------------------------------------------------------------------------------------
set(OD_TP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../third_party)

# The Group5 collision, resolved by source selection rather than by a linker flag.
#
# bb_epaper and FastEPD both ship Group5.cpp -- same author, diverged copies, and NOT the same
# licence header (bb_epaper's carries SPDX GPL-3.0-or-later; FastEPD's does not). The source
# repo linked both and passed -Wl,--allow-multiple-definition, which makes the effective G5
# codec depend on link order and hides any divergence between the copies.
#
# MEMORY_CONSTRAINTS.md § "Open questions" recommends option 1 -- compile the duplicated file
# from ONE library -- and DESIGN_REVIEW § "Likely pitfalls" #4 warns the flag must not ship
# "temporarily", because temporary linker workarounds become permanent ones. So: on a FastEPD
# board, Group5 comes from FastEPD and bb_epaper's copy is excluded; otherwise only bb_epaper
# is compiled and the question does not arise. No --allow-multiple-definition anywhere.
file(GLOB OD_BBEP_SOURCES ${OD_TP_DIR}/bb_epaper/src/*.cpp)
# Kept unfiltered: every bb_epaper TU needs its own header directory on its include path, and
# which ones are COMPILED depends on the board branch below. Grant on the superset.
set(OD_BBEP_ALL_SOURCES ${OD_BBEP_SOURCES})

# bb_epaper.cpp is EXCLUDED and replaced by ../panel/od_bbep.cpp.
#
# That file is 827 lines: 52 of glue (includes, the *_io.inl selection #ifdef chain, then
# bb_ep.inl + bb_ep_gfx.inl) and 775 of the BBEPAPER C++ class, which this project never
# references -- display_service.cpp drives a raw BBEPDISP through the bare C functions.
#
# Replacing the glue rather than patching its #ifdef chain (how Firmware_NRF54 selects its
# backend) means ZERO edits to vendored files to use our own IO backend, so nothing new joins
# third_party/NOTICE.md's re-verify-on-every-bump list. Dropping the class also removes the only
# consumer of pinMode() and millis(), which is why panel/od_bbep_idf_io.inl defines neither.
#
# Group5.cpp stays: it is a separate TU providing the G5 codec, and the FastEPD collision
# handling below still applies to it.
#
# See docs/BBEPAPER_IO_BACKENDS.md and panel/od_bbep.cpp.
list(FILTER OD_BBEP_SOURCES EXCLUDE REGEX "/bb_epaper\\.cpp$")
list(APPEND OD_APP_SOURCES ${CMAKE_CURRENT_LIST_DIR}/../panel/od_bbep.cpp)

# Which boards use FastEPD, keyed on OD_BOARD alone.
#
# This mapping would sit more naturally in boards/<board>.cmake, and it is here for a reason
# worth recording: IDF expands component CMakeLists in a SEPARATE early cmake invocation whose
# cache carries none of the parent project's variables -- not OD_BOARD_DEFINES, not even a
# CACHE INTERNAL set by the parent. A conditional source list keyed on those silently takes
# the wrong branch in that pass. Including the board fragment here instead makes IDF's own
# property serialisation emit a dev warning, which IDF then treats as a fatal error.
#
# OD_BOARD is a -D on the command line, so component expansion sees it and the source list is
# consistent there. It is NOT visible everywhere: component_get_requirements.cmake is a separate
# script include with its own scope, and OD_BOARD is empty inside it -- which is why the assertion
# below returns early rather than resolving a path from it.
# FastEPD is ESP32-S3-only and PSRAM-mandatory (MEMORY_CONSTRAINTS.md
# § "FastEPD is ESP32-only and PSRAM-mandatory"), so this list can only ever hold S3 boards.
# Boards whose source env sets -DOPENDISPLAY_FASTEPD *and* lists the FastEPD lib_dep.
# Read from platformio.ini, not guessed -- the two are not the same set:
#
#   s3-e1004 was WRONG in this list. env:esp32-s3-E1004 extends env:esp32-s3-N32R8-extuart,
#   which sets neither the define nor the lib_dep; that board's panel goes through bb_epaper.
#   Listing it here would have given it FastEPD's Group5 and excluded bb_epaper's, changing
#   which copy wins the collision resolved below.
#
#   s3-n8r8 and the two s3-n16r8-extuart boards were MISSING. All three set the define.
set(OD_FASTEPD_BOARDS
    s3-n16r8
    s3-n8r8
    s3-n32r8
    s3-n16r8-extuart
    s3-n16r8-extuart-debug)

# Cross-check the hand-maintained list against the fragment it mirrors. The list exists because
# the fragment's VARIABLES are invisible in this pass (see above) -- its TEXT is not, and reading
# it is the whole assertion. Comment lines are stripped first: three non-FastEPD fragments name
# the macro in prose explaining why they do not set it, and matching those would invert the check.
#
# Inside a function so nothing it needs survives into directory scope. IDF serialises this
# component's variables into build_properties.temp.cmake during the early requirements pass, and
# raw file text there produces a dev warning that IDF escalates to a fatal error -- the same trap
# the note above describes for OD_BOARD_DEFINES.
function(od_assert_fastepd_list board fragment)
    # OD_BOARD is empty in IDF's component_get_requirements pass -- that script include has its
    # own scope and does not inherit the cache the way component expansion does. Nothing there
    # depends on this mapping; the assertion runs in the pass that builds the source list.
    if(board STREQUAL "")
        return()
    endif()
    if(NOT EXISTS "${fragment}")
        message(FATAL_ERROR "od: no board fragment at ${fragment}")
    endif()
    file(STRINGS "${fragment}" fastepd_lines REGEX "^[^#]*OPENDISPLAY_FASTEPD")
    set(fragment_wants FALSE)
    if(fastepd_lines)
        set(fragment_wants TRUE)
    endif()
    set(list_wants FALSE)
    if(board IN_LIST OD_FASTEPD_BOARDS)
        set(list_wants TRUE)
    endif()
    if(NOT fragment_wants STREQUAL list_wants)
        message(FATAL_ERROR
            "od: OD_FASTEPD_BOARDS says ${list_wants} for '${board}' but its fragment says "
            "${fragment_wants}. These decide which library's Group5.cpp wins the link, so they "
            "must agree.")
    endif()
endfunction()

od_assert_fastepd_list("${OD_BOARD}" "${CMAKE_CURRENT_LIST_DIR}/../boards/${OD_BOARD}.cmake")

if(OD_BOARD IN_LIST OD_FASTEPD_BOARDS)
    list(APPEND OD_APP_SOURCES ${OD_TP_DIR}/FastEPD/src/FastEPD.cpp
                               ${OD_TP_DIR}/FastEPD/src/Group5.cpp)
    list(FILTER OD_BBEP_SOURCES EXCLUDE REGEX "/Group5\\.cpp$")
    list(APPEND OD_APP_SOURCES ${OD_BBEP_SOURCES})
    message(STATUS "od: FastEPD board -- Group5 from FastEPD, bb_epaper's copy excluded")
else()
    list(APPEND OD_APP_SOURCES ${OD_BBEP_SOURCES})
    message(STATUS "od: non-FastEPD board -- Group5 from bb_epaper")
endif()

idf_component_register(
    SRCS ${OD_APP_SOURCES}
    INCLUDE_DIRS
        ${OD_SRC_DIR}
        # NOTE: ../vendor/fastepd is deliberately ABSENT here -- see the scoping block below.
        ${CMAKE_CURRENT_LIST_DIR}/../hal
        ${CMAKE_CURRENT_LIST_DIR}/../ble
        # NOTE: neither third_party/bb_epaper/src nor third_party/FastEPD/src is here, and
        # nor is ../vendor/fastepd. All three are granted per-source below. ../panel stays --
        # it is OUR code (the IDF backend), not a vendored tree.
        ${CMAKE_CURRENT_LIST_DIR}/../panel
        # shared/ -- the wire contract AND, since 2026-08-05, shared sources and their headers.
        # Consumed directly, NOT copied into this target: that is the whole reason phase A left
        # include/opendisplay_protocol.h behind. The list comes from shared/sources.cmake so
        # this target cannot drift from the host build's view of shared/.
        ${OD_SHARED_INCLUDE_DIRS}
    REQUIRES
        app_update bt nvs_flash esp_timer driver esp_wifi esp_event mbedtls spiffs vfs esp_adc
        # esp_lcd: FastEPD drives the S3 LCD peripheral for parallel e-paper
        esp_lcd esp_psram
        # mdns: vendored under ../components/mdns (ESP-IDF dropped it from core in v5.0).
        # Listed UNCONDITIONALLY even though only OPENDISPLAY_ENABLE_WIFI boards call it.
        # Gating it on the board would make the component list board-dependent, and IDF
        # resolves REQUIRES before the board fragment's defines exist; the boards that do
        # not use it never reference a symbol, so the linker discards the archive and they
        # pay nothing. Verified by comparing esp32-n4's image size across this change.
        mdns
)

# ---------------------------------------------------------------------------------------
# PANEL LIBRARIES AND THE FASTEPD ADAPTER ARE OUT OF GLOBAL SCOPE
# ---------------------------------------------------------------------------------------
# Three directories that used to sit on the component's include path and no longer do:
#
#   third_party/bb_epaper/src   the bb_epaper panel library
#   third_party/FastEPD/src     the FastEPD panel library
#   vendor/fastepd              THE FASTEPD VENDOR ADAPTER -- an Arduino SPI object over IDF's
#                               spi_master. Permanent, not a shim: FastEPD's IT8951 transport
#                               is written against that object and there is no version of it
#                               that is not. compat/ died; this did not.
#
# While they were global, ANY source in the component could reach a vendor panel API, or
# acquire an Arduino SPI dependency, by typing one include. Each is now granted to a NAMED LIST
# of translation units, so gaining a consumer is an explicit edit here rather than a side
# effect of an include line. Verified by adding <SPI.h> to an unlisted file: it fails to
# compile, which is the property this block exists to create.
#
# THIS IS CONTAINMENT, NOT ABSTRACTION, and the difference matters. The vendor types still
# appear in the listed files -- display_service.cpp drives a raw BBEPDISP, display_fastepd.cpp
# drives a FASTEPD. The destination is od_hal_panel with the od_panel_ops vtable that
# docs/SHARED_API_DESIGN.md specifies and CLAUDE.md names as the ONE deliberate function-pointer
# exception, precisely because this target legitimately has two panel backends. Then the lists
# below collapse to the two backend files and nothing else sees a vendor header at all.
#
# bb_epaper needs NOTHING from vendor/fastepd: panel/od_bbep.cpp replaces the vendored
# bb_epaper.cpp with our own IDF backend (panel/od_bbep_idf_io.inl), so arduino_io.inl and
# esphome_io.inl are never compiled and bb_epaper.h's <Arduino.h> sits behind an #ifdef ARDUINO
# this build does not define.
set(OD_FASTEPD_ADAPTER_DIR ${CMAKE_CURRENT_LIST_DIR}/../vendor/fastepd)
set(OD_BBEP_INC_DIR        ${OD_TP_DIR}/bb_epaper/src)
set(OD_QRCODE_INC_DIR      ${OD_TP_DIR})
set(OD_FASTEPD_INC_DIR     ${OD_TP_DIR}/FastEPD/src)

# Per-file grants. A file appears once, with every directory it needs -- the INCLUDE_DIRECTORIES
# source property REPLACES rather than appends, so a second set_source_files_properties() call
# on the same file would silently drop the first grant.
#
#   display_service.cpp   bb_epaper ONLY. It held an adapter grant until 2026-08-04, for the
#                         E1004 dual-CS pixel stream, which opened a SECOND device on
#                         SPI2_HOST -- the host panel/od_bbep_idf_io.inl already owns. That
#                         payload now goes through od_bbep_stream_write(), so there is ONE
#                         owner of the host and this file has no adapter dependency at all.
#                         Do not re-add it: <SPI.h> here means a second bus owner is back.
#   display_fastepd.cpp   FastEPD + adapter. FastEPD glue by definition.
#   boot_screen.cpp,      bb_epaper only -- both draw through BBEPDISP.
#   main.cpp              (main.h defines the `BBEPDISP bbep` global.)
set_source_files_properties(${OD_SRC_DIR}/display_service.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_BBEP_INC_DIR}")
set_source_files_properties(${OD_SRC_DIR}/display_fastepd.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_FASTEPD_INC_DIR};${OD_FASTEPD_ADAPTER_DIR}")
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../panel/od_panel_fastepd.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_FASTEPD_INC_DIR};${OD_FASTEPD_ADAPTER_DIR}")
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../panel/od_panel_bbep.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_BBEP_INC_DIR}")
set_source_files_properties(${OD_SRC_DIR}/boot_screen.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_BBEP_INC_DIR};${OD_QRCODE_INC_DIR}")
set_source_files_properties(${OD_SRC_DIR}/main.cpp
                            ${CMAKE_CURRENT_LIST_DIR}/../panel/od_bbep.cpp
                            ${OD_BBEP_ALL_SOURCES}
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_BBEP_INC_DIR}")
set_source_files_properties(${CMAKE_CURRENT_LIST_DIR}/../vendor/fastepd/fastepd_adapter.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_FASTEPD_ADAPTER_DIR}")
if(OD_BOARD IN_LIST OD_FASTEPD_BOARDS)
    set_source_files_properties(${OD_TP_DIR}/FastEPD/src/FastEPD.cpp
        TARGET_DIRECTORY ${COMPONENT_LIB}
        PROPERTIES INCLUDE_DIRECTORIES "${OD_FASTEPD_INC_DIR};${OD_FASTEPD_ADAPTER_DIR}")
    set_source_files_properties(${OD_TP_DIR}/FastEPD/src/Group5.cpp
        TARGET_DIRECTORY ${COMPONENT_LIB}
        PROPERTIES INCLUDE_DIRECTORIES "${OD_FASTEPD_INC_DIR}")
endif()

# Board defines from boards/<board>.cmake. These are the surviving half of the source repo's
# build_flags: the -D flags that are genuinely application configuration, as opposed to the
# Arduino/PlatformIO plumbing (ARDUINO_USB_*, board_build.*) which became sdkconfig instead.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ${OD_BOARD_DEFINES}
    OD_XFER_DIRECT_AUTO_END=1
    # This target has no tag. It must be set HERE, with the commit that adds od_nfc.c to the
    # aggregate: od_core_reset() names od_nfc_reset(), so the machine is linked from that moment
    # and a capability-on build would demand an od_nfc_app implementation ESP32 has never had.
    OD_CAP_NFC=0)

# FastEPD selects its IO backend with `#ifdef ARDUINO / #elif defined(__LINUX__)`. Under IDF
# NEITHER was defined, so every it8951 transport function in FastEPD.inl compiled to an EMPTY
# BODY between two gpio_set_level() calls: commands sent nothing, pixel writes sent nothing,
# and it8951ReadData() returned 0 -- while the GPIO reset/busy handshake still looked healthy.
# It linked and it booted; the panel simply never received a byte.
#
# OD_FASTEPD_IDF_SPI selects a THIRD branch, added as an OD-PATCH in FastEPD.inl, that drives
# the transport through vendor/fastepd/SPI.h. Defining ARDUINO instead was tried and rejected: it also
# selects FastEPD's Arduino font/Print/PROGMEM surface (pgm_read_*, memcpy_P, ledcAttach,
# TwoWire::setTimeout), which is a whole Arduino core rather than an IO backend, and bb_epaper
# reads the same macro and must keep its esp_idf backend.
#
# Both this define and the patch disappear when FastEPD gains a real esp_idf IO backend
# upstream. See third_party/NOTICE.md § "FastEPD has no ESP-IDF IO backend".
if(OD_BOARD IN_LIST OD_FASTEPD_BOARDS)
    set_source_files_properties(${OD_TP_DIR}/FastEPD/src/FastEPD.cpp
        TARGET_DIRECTORY ${COMPONENT_LIB}
        PROPERTIES COMPILE_DEFINITIONS "OD_FASTEPD_IDF_SPI=1")
endif()

# The imported sources are Arduino C++ and will not be warning-clean under IDF's defaults for
# some time. Phase B's goal is to LINK AND BOOT, so warnings stay warnings here -- unlike
# shared/, which is -Werror from its first line (tests/host/CMakeLists.txt). Tightening this
# is phase C work, and it should be tightened: leaving it permanently is how the imported half
# of the tree stays permanently unowned.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-function)
