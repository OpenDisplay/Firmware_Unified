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
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_wdt.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_crypto.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_crypto_random.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_secret.c
                           ${CMAKE_CURRENT_LIST_DIR}/../hal/od_hal_radio.cpp
                           ${CMAKE_CURRENT_LIST_DIR}/../ble/od_ble_nimble.cpp)

# Shared sources: the whole aggregate, because this target implements every HAL tier.
# od_hal_adv_{program,start,stop} are at the bottom of ble/od_ble_nimble.cpp; od_hal_wdt is
# hal/od_hal_wdt.c, listed above. Taking the aggregate rather than naming tiers is what makes a
# new shared source reach this build without an edit here.
list(APPEND OD_APP_SOURCES ${OD_SHARED_SOURCES})
# Every SPI panel is driven from the config's drive packets (od_display, src/od_display_esp.cpp):
# uploads, partials, the boot screen, CMD_DRIVE_WRITE_START and CMD_SPI_RAW. No panel library.
list(APPEND OD_APP_SOURCES ${OD_SHARED_SOURCES_APP_EPD} ${OD_SHARED_SOURCES_APP_DRIVE}
                           ${OD_SHARED_SOURCES_APP_DISPLAY} ${OD_SHARED_SOURCES_APP_IDENT}
                           ${OD_SHARED_SOURCES_APP_SPI_RAW})
# The 0xB2 action table (od_action + CMD_ACTION_INFO / CMD_ACTION_FIRE); effects in
# src/od_action_app_esp.cpp.
list(APPEND OD_APP_SOURCES ${OD_SHARED_SOURCES_APP_ACTION})

# ble/od_ble_rx.cpp is GONE, not forgotten. It held od_ble_on_write(): the bounds check,
# the RX hex log and the enqueue. All three now live in src/command_queue.cpp's
# bleRxQueuePush(), which both targets share -- keeping a second copy here is what let the
# two transports report the same malformed frame differently.

# ---------------------------------------------------------------------------------------
# Panel libraries, vendored -- see ../../../third_party/NOTICE.md for origin and licence.
# ---------------------------------------------------------------------------------------
set(OD_TP_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../third_party)

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
# FastEPD is PSRAM-mandatory (MEMORY_CONSTRAINTS.md § "FastEPD is ESP32-only and
# PSRAM-mandatory"). Its parallel bus is esp_lcd i80, which the S3 drives with its LCD
# peripheral and the classic ESP32 with I2S in LCD mode -- so classic-ESP32 PSRAM boards
# (Inkplate, M5Paper) belong here too.
# Boards whose source env sets -DOPENDISPLAY_FASTEPD *and* lists the FastEPD lib_dep.
# Read from platformio.ini, not guessed -- the two are not the same set:
#
#   s3-e1004 is not in this list: env:esp32-s3-E1004 extends env:esp32-s3-N32R8-extuart, which
#   sets neither the define nor the lib_dep; its panel is an SPI panel driven from drive packets.
#
#   s3-n8r8 and the two s3-n16r8-extuart boards were MISSING. All three set the define.
set(OD_FASTEPD_BOARDS
    s3-n16r8
    s3-n8r8
    s3-n32r8
    s3-n16r8-extuart
    s3-n16r8-extuart-debug
    esp32-wrover-e-n4r8)

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
            "${fragment_wants}. These decide whether FastEPD is compiled, so they must agree.")
    endif()
endfunction()

od_assert_fastepd_list("${OD_BOARD}" "${CMAKE_CURRENT_LIST_DIR}/../boards/${OD_BOARD}.cmake")

if(OD_BOARD IN_LIST OD_FASTEPD_BOARDS)
    list(APPEND OD_APP_SOURCES ${OD_TP_DIR}/FastEPD/src/FastEPD.cpp
                               ${OD_TP_DIR}/FastEPD/src/Group5.cpp)
    message(STATUS "od: FastEPD board -- IT8951 / parallel panels on FastEPD")
endif()

# Factory provisioning, the IDF form of the PlatformIO pre-script scripts/factory_config_gen.py:
#   OPENDISPLAY_FACTORY_CONFIG_HEX="BD 00 01 ..." idf.py ... reconfigure build
#       embeds a config packet that the first boot with no stored config saves to NVS
#       (tryProvisionFactoryEmbed()).
#   OPENDISPLAY_FACTORY_CLEAR_CONFIG=1 idf.py ... reconfigure build
#       builds a one-shot image that erases the stored config on every boot.
# -DOD_FACTORY_CONFIG_HEX / -DOD_FACTORY_CLEAR_CONFIG=1 work the same. Both are read at
# CONFIGURE time and deliberately not kept in the cache: a build directory reused for a
# normal image must not keep shipping a factory config, so every reconfigure without them
# drops the embed. The generated source lives in the build tree, never in src/.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(od_factory_hex "$ENV{OPENDISPLAY_FACTORY_CONFIG_HEX}")
    if(DEFINED OD_FACTORY_CONFIG_HEX)
        set(od_factory_hex "${OD_FACTORY_CONFIG_HEX}")
        unset(OD_FACTORY_CONFIG_HEX CACHE)
    endif()
    set(od_factory_clear "$ENV{OPENDISPLAY_FACTORY_CLEAR_CONFIG}")
    if(DEFINED OD_FACTORY_CLEAR_CONFIG)
        set(od_factory_clear "${OD_FACTORY_CLEAR_CONFIG}")
        unset(OD_FACTORY_CLEAR_CONFIG CACHE)
    endif()
    set(OD_FACTORY_DEFINES "")
    if(od_factory_clear MATCHES "^(1|[Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]|[Oo][Nn])$")
        list(APPEND OD_FACTORY_DEFINES FACTORY_CLEAR_CONFIG_ON_BOOT=1)
        message(STATUS "od: factory clear build (FACTORY_CLEAR_CONFIG_ON_BOOT)")
    elseif(NOT od_factory_hex STREQUAL "")
        idf_build_get_property(od_python PYTHON)
        set(od_factory_cpp "${CMAKE_BINARY_DIR}/od_generated/factory_config_data.cpp")
        execute_process(
            COMMAND ${od_python} ${CMAKE_CURRENT_LIST_DIR}/../tools/factory_config_embed.py
                    --out "${od_factory_cpp}" --hex "${od_factory_hex}"
            RESULT_VARIABLE od_factory_rc)
        if(NOT od_factory_rc EQUAL 0)
            message(FATAL_ERROR "od: factory config packet rejected (see above)")
        endif()
        list(APPEND OD_APP_SOURCES ${od_factory_cpp})
        list(APPEND OD_FACTORY_DEFINES FACTORY_HAS_EMBED=1)
    endif()
endif()

# TEST-ONLY SEC2 trace (shared/core/od_sec2_trace.h): prints the claim code, the setup-screen
# kind, claim-mode transitions, boot-history decisions and factory-reset steps on the log
# console. The claim code is what claim mode keeps off the radio, so this is a bench build only:
#   -DOD_SEC2_TEST_TRACE=I_ACCEPT_CLAIM_CODE_ON_SERIAL     (any other value is a configure error)
#   -DOD_SEC2_TEST_CM_WINDOW_MS=<ms>                       (optional, only with the trace: a
#                                                           shorter claim-mode window)
# Not kept in the cache, like the factory switches above: a reconfigure without the flag builds a
# normal image. Refused when OD_FW_VERSION or CI is set (build-release.sh, release.yml).
# The same names are also read from the environment (the form the Nordic target takes, see
# targets/nordic-zephyr/zephyr/CMakeLists.txt), so one bench recipe builds both targets.
foreach(od_test_opt OD_SEC2_TEST_TRACE OD_SEC2_TEST_CM_WINDOW_MS)
    if(NOT DEFINED ${od_test_opt} AND DEFINED ENV{${od_test_opt}})
        set(${od_test_opt} "$ENV{${od_test_opt}}")
    endif()
endforeach()
set(OD_SEC2_TRACE_DEFINES "")
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND DEFINED OD_SEC2_TEST_TRACE)
    set(od_sec2_trace "${OD_SEC2_TEST_TRACE}")
    unset(OD_SEC2_TEST_TRACE CACHE)
    if(NOT od_sec2_trace STREQUAL "" AND NOT od_sec2_trace MATCHES "^(0|OFF|[Nn][Oo])$")
        if(NOT od_sec2_trace STREQUAL "I_ACCEPT_CLAIM_CODE_ON_SERIAL")
            message(FATAL_ERROR "od: OD_SEC2_TEST_TRACE must be I_ACCEPT_CLAIM_CODE_ON_SERIAL "
                                "(a test-only build that prints the claim code)")
        endif()
        if(DEFINED ENV{OD_FW_VERSION} OR DEFINED ENV{CI})
            message(FATAL_ERROR "od: OD_SEC2_TEST_TRACE refused in a release/CI build")
        endif()
        list(APPEND OD_SEC2_TRACE_DEFINES OD_SEC2_TEST_TRACE=1)
        message(WARNING "od: SEC2 TEST-TRACE BUILD -- prints the claim code on the console. NOT FOR RELEASE.")
    endif()
endif()
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND DEFINED OD_SEC2_TEST_CM_WINDOW_MS)
    set(od_sec2_window "${OD_SEC2_TEST_CM_WINDOW_MS}")
    unset(OD_SEC2_TEST_CM_WINDOW_MS CACHE)
    if(NOT od_sec2_window STREQUAL "")
        if(NOT OD_SEC2_TRACE_DEFINES)
            message(FATAL_ERROR "od: OD_SEC2_TEST_CM_WINDOW_MS needs OD_SEC2_TEST_TRACE")
        endif()
        if(NOT od_sec2_window MATCHES "^[0-9]+$")
            message(FATAL_ERROR "od: OD_SEC2_TEST_CM_WINDOW_MS must be a number of milliseconds")
        endif()
        list(APPEND OD_SEC2_TRACE_DEFINES OD_CM_WINDOW_MS=${od_sec2_window}u)
        message(WARNING "od: test claim-mode window ${od_sec2_window} ms")
    endif()
endif()

idf_component_register(
    SRCS ${OD_APP_SOURCES}
    INCLUDE_DIRS
        ${OD_SRC_DIR}
        # NOTE: ../vendor/fastepd is deliberately ABSENT here -- see the scoping block below.
        ${CMAKE_CURRENT_LIST_DIR}/../hal
        ${CMAKE_CURRENT_LIST_DIR}/../ble
        # NOTE: neither third_party/FastEPD/src nor ../vendor/fastepd is here. Both are granted
        # per-source below.
        # shared/ -- the wire contract AND, since 2026-08-05, shared sources and their headers.
        # Consumed directly, NOT copied into this target: that is the whole reason phase A left
        # include/opendisplay_protocol.h behind. The list comes from shared/sources.cmake so
        # this target cannot drift from the host build's view of shared/.
        ${OD_SHARED_INCLUDE_DIRS}
    REQUIRES
        app_update bt nvs_flash esp_timer driver esp_wifi esp_event mbedtls spiffs vfs esp_adc
        # esp_lcd: FastEPD drives the S3 LCD peripheral for parallel e-paper
        esp_lcd esp_psram
        # esp_pm: the M5Paper CPU clamp (only with CONFIG_PM_ENABLE)
        esp_pm
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
# Two directories kept off the component's include path:
#
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
# SPI panels need no vendor header at all: od_display drives them from the config's drive
# packets through src/od_epd_esp.cpp. tools/check.sh ("esp32: no panel library") keeps it so.
set(OD_FASTEPD_ADAPTER_DIR ${CMAKE_CURRENT_LIST_DIR}/../vendor/fastepd)
set(OD_QRCODE_INC_DIR      ${OD_TP_DIR})
set(OD_FASTEPD_INC_DIR     ${OD_TP_DIR}/FastEPD/src)

# Per-file grants. A file appears once, with every directory it needs -- the INCLUDE_DIRECTORIES
# source property REPLACES rather than appends, so a second set_source_files_properties() call
# on the same file would silently drop the first grant.
#
#   display_fastepd.cpp   FastEPD + adapter. FastEPD glue by definition.
#   boot_screen.cpp       the QR encoder.
set_source_files_properties(${OD_SRC_DIR}/display_fastepd.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_FASTEPD_INC_DIR};${OD_FASTEPD_ADAPTER_DIR}")
set_source_files_properties(${OD_SRC_DIR}/boot_screen.cpp
    TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES INCLUDE_DIRECTORIES "${OD_QRCODE_INC_DIR}")
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
    ${OD_FACTORY_DEFINES}
    ${OD_SEC2_TRACE_DEFINES}
    OD_XFER_DIRECT_AUTO_END=1
    # This target has no tag. It must be set HERE, with the commit that adds od_nfc.c to the
    # aggregate: od_core_reset() names od_nfc_reset(), so the machine is linked from that moment
    # and a capability-on build would demand an od_nfc_app implementation ESP32 has never had.
    OD_CAP_NFC=0
    # 0x0085 and 0x0084 run on od_epd (src/od_display_esp.cpp); SPI panels only.
    OD_CAP_DRIVE_WRITE=1
    OD_CAP_SPI_RAW=1
    OD_CAP_READ_IDENT=1
    # 0xB2 action table without an image store: DISPLAY_SLOT entries load as inactive.
    OD_CAP_ACTION=1
    OD_IDENT_TARGET=OD_IDENT_TARGET_ESP32
    # IT8951 / FastEPD-parallel panels are driven by a built-in driver: no NEEDS_MIGRATION.
    OD_IDENT_NATIVE_PANELS=1
    # boot_screen.cpp picks the od_display sink or the FastEPD framebuffer per frame.
    OD_DISPLAY_SHARED_BOOT_SINK=0
    # Dual-controller panels: the drive stream walks a split frame by IMAGE_SLICE, one CS each.
    OD_XFER_SPLIT_HALVES=1
    # 0xB3 display_controller selects IT8951 / parallel panels' built-in driver.
    OD_CFG_INDEX_CONTROLLER=1)

# FastEPD selects its IO backend with `#ifdef ARDUINO / #elif defined(__LINUX__)`. Under IDF
# NEITHER was defined, so every it8951 transport function in FastEPD.inl compiled to an EMPTY
# BODY between two gpio_set_level() calls: commands sent nothing, pixel writes sent nothing,
# and it8951ReadData() returned 0 -- while the GPIO reset/busy handshake still looked healthy.
# It linked and it booted; the panel simply never received a byte.
#
# OD_FASTEPD_IDF_SPI selects a THIRD branch, added as an OD-PATCH in FastEPD.inl, that drives
# the transport through vendor/fastepd/SPI.h. Defining ARDUINO instead was tried and rejected: it also
# selects FastEPD's Arduino font/Print/PROGMEM surface (pgm_read_*, memcpy_P, ledcAttach,
# TwoWire::setTimeout), which is a whole Arduino core rather than an IO backend.
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
