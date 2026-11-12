cmake_minimum_required(VERSION 3.13.1)

set(PROJECT_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
# shared/ -- ONE source list, four consumers (shared/sources.cmake). Sets OD_SHARED_SOURCES,
# the per-tier lists, and OD_SHARED_INCLUDE_DIRS. Do not glob shared/ from this file.
include(${PROJECT_ROOT}/../../shared/sources.cmake)
# third_party is the REPO-LEVEL vendored tree, shared by every target -- not a per-target copy.
# The source repo had its own third_party/ beside src/; importing that would have made this
# repo's "one vendored copy for all targets" rule a fiction. ../../.. is the repo root.
set(THIRD_PARTY_DIR ${PROJECT_ROOT}/../../third_party)
set(PANEL_DIR ${PROJECT_ROOT}/panel)
set(SRC_DIR ${PROJECT_ROOT}/src)
set(GEN_DIR ${SRC_DIR}/generated)

list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_ncs.conf)

# NO CHANNEL SOUNDING CONF HERE. CS is an nRF54L radio feature and the nRF52840 has none, so
# appending it unconditionally makes every CS symbol report "assigned y but got n" -- fatal in a
# build that promotes Kconfig warnings to errors. It lives in boards/<board>.conf for the two
# nRF54 boards.
#
# Guarding the append on ${BOARD} is only safe when the board comes from the command line:
# `west build -b <board>` sets BOARD in the cache before this file runs, but a build that leaves
# it to find_package(Zephyr) -- which runs below -- sees it empty here and takes the wrong arm.
# Do not add a CS guard on that basis; boards/<board>.conf is where per-board Kconfig belongs.

# LIVE, despite an earlier comment here calling it dead. `west build -b xiao_nrf54lm20a` sets
# BOARD before this runs, and build-nrf54lm20-debug/zephyr/build_info.yml records
# prj_lm20_extra.conf among the applied conf files. Its content is ALSO mirrored into
# boards/xiao_nrf54lm20a_nrf54lm20a_cpuapp.conf, so the two must stay in step -- that redundancy
# is what made the branch look dead. Do not delete either on the strength of the other.
if(BOARD MATCHES ".*lm20.*")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_lm20_extra.conf)
endif()

if(DEFINED ENV{PROFILE} AND "$ENV{PROFILE}" STREQUAL "uart")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_uart.conf)
elseif(OD_UART_DEBUG OR (DEFINED PROFILE AND PROFILE STREQUAL "uart"))
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_uart.conf)
endif()

if(DEFINED ENV{PROFILE} AND "$ENV{PROFILE}" STREQUAL "quiet")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_quiet.conf)
  set(OD_LOW_POWER_QUIET ON)
elseif(OD_LOW_POWER_QUIET OR (DEFINED PROFILE AND PROFILE STREQUAL "quiet"))
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_quiet.conf)
  set(OD_LOW_POWER_QUIET ON)
endif()

if(DEFINED ENV{PROFILE} AND "$ENV{PROFILE}" STREQUAL "debug")
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_debug.conf)
  set(OD_DEBUG_BUILD ON)
elseif(OD_DEBUG_BUILD OR (DEFINED PROFILE AND PROFILE STREQUAL "debug"))
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_debug.conf)
  set(OD_DEBUG_BUILD ON)
endif()

# ------------------------------------------------------------- TEST-ONLY bench build ---
# Test-only build options share ONE naming scheme on every target, OD_SEC2_TEST_<what>, and one
# ratchet (tools/check.sh "release: test-only builds off in every release configuration"). Here
# they are read from the ENVIRONMENT only (sysbuild does not forward a plain -D to the app image;
# never a cache variable, never a board file), so a reconfigure without them builds a release
# image again:
#   OD_SEC2_TEST_TRACE=I_ACCEPT_CLAIM_CODE_ON_SERIAL   print the SEC2 trace (claim code, screen
#                                                       kinds, boot-history decisions) on the
#                                                       USB console (shared/core/od_sec2_trace.h)
#   OD_SEC2_TEST_REVERT_S=<seconds>                     no-touch way back: an unconfirmed image
#                                                       resets after <seconds> so MCUboot
#                                                       reverts it (src/od_test_bench.c)
# Either one adds src/od_test_bench.c (console commands: od stat|confirm|blind|reboot|por|pin|dfu|
# panic|hang), prj_test_bench.conf and the derived define OD_SEC2_BENCH_BUILD. Both are refused in
# a versioned (OD_FW_VERSION) or CI build, like the ESP32 options in
# targets/esp32-idf/main/CMakeLists.txt.
set(OD_BENCH_DEFINES "")
set(od_bench_build OFF)
if(DEFINED ENV{OD_SEC2_TEST_TRACE} AND NOT "$ENV{OD_SEC2_TEST_TRACE}" MATCHES "^(|0|OFF|[Nn][Oo])$")
  if(NOT "$ENV{OD_SEC2_TEST_TRACE}" STREQUAL "I_ACCEPT_CLAIM_CODE_ON_SERIAL")
    message(FATAL_ERROR "od: OD_SEC2_TEST_TRACE must be I_ACCEPT_CLAIM_CODE_ON_SERIAL "
                        "(a test-only build that prints the claim code)")
  endif()
  list(APPEND OD_BENCH_DEFINES OD_SEC2_TEST_TRACE=1)
  set(od_bench_build ON)
  message(WARNING "od: SEC2 TEST-TRACE BUILD -- prints the claim code on the console. NOT FOR RELEASE.")
endif()
if(DEFINED ENV{OD_SEC2_TEST_REVERT_S} AND NOT "$ENV{OD_SEC2_TEST_REVERT_S}" STREQUAL "")
  if(NOT "$ENV{OD_SEC2_TEST_REVERT_S}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "od: OD_SEC2_TEST_REVERT_S must be a positive number of seconds")
  endif()
  list(APPEND OD_BENCH_DEFINES OD_SEC2_TEST_REVERT_S=$ENV{OD_SEC2_TEST_REVERT_S}u)
  set(od_bench_build ON)
  message(WARNING "od: TEST IMAGE -- does not confirm itself; resets for an MCUboot revert after "
                  "$ENV{OD_SEC2_TEST_REVERT_S} s unless confirmed. NOT FOR RELEASE.")
endif()
if(od_bench_build)
  if(DEFINED ENV{OD_FW_VERSION} OR DEFINED ENV{CI})
    message(FATAL_ERROR "od: bench build (OD_SEC2_TEST_TRACE / OD_SEC2_TEST_REVERT_S) refused in a "
                        "release/CI build")
  endif()
  list(APPEND OD_BENCH_DEFINES OD_SEC2_BENCH_BUILD=1)
  list(APPEND EXTRA_CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_test_bench.conf)
endif()

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(opendisplay_nordic)

# This is a project build option, not Kconfig. Pull it through the sysbuild cache so the
# qualification image can prove that no fallback code was linked.
zephyr_get(OD_EPD_SPI_REQUIRE_SPIM)

set(FACTORY_GEN_SCRIPT ${PROJECT_ROOT}/scripts/factory_config_gen.py)
set(FACTORY_GEN_STAMP ${GEN_DIR}/.factory_gen_stamp)

if(NOT DEFINED FACTORY_CONFIG_HEX)
  set(FACTORY_CONFIG_HEX "")
endif()
if(NOT DEFINED FACTORY_CLEAR_CONFIG_ON_BOOT)
  set(FACTORY_CLEAR_CONFIG_ON_BOOT OFF)
endif()

set(FACTORY_GEN_ARGS "")
if(FACTORY_CONFIG_HEX)
  list(APPEND FACTORY_GEN_ARGS --hex=${FACTORY_CONFIG_HEX})
endif()
if(FACTORY_CLEAR_CONFIG_ON_BOOT)
  list(APPEND FACTORY_GEN_ARGS --clear)
endif()

add_custom_command(
  OUTPUT ${FACTORY_GEN_STAMP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN_DIR}
  COMMAND ${Python3_EXECUTABLE} ${FACTORY_GEN_SCRIPT} ${FACTORY_GEN_ARGS}
  COMMAND ${CMAKE_COMMAND} -E touch ${FACTORY_GEN_STAMP}
  DEPENDS ${FACTORY_GEN_SCRIPT} ${PROJECT_ROOT}/tools/config_packet.py
  WORKING_DIRECTORY ${PROJECT_ROOT}
  COMMENT "Generating factory_config_data.c"
  VERBATIM
)

add_custom_target(factory_config_gen DEPENDS ${FACTORY_GEN_STAMP})

# OD_SHARED_INCLUDE_DIRS FIRST, and that is a rule rather than a formatting choice: it carries
# the canonical opendisplay_protocol.h / opendisplay_structs.h. ${SRC_DIR} used to carry
# hand-written subsets of both, which shadowed them; those are gone (src/od_runtime_types.h now
# holds the RAM-only types they also carried), and keeping shared/ ahead of src/ is what stops
# another copy from quietly taking over if one reappears.
target_include_directories(app PRIVATE
  ${OD_SHARED_INCLUDE_DIRS}
  ${SRC_DIR}
  ${THIRD_PARTY_DIR}
  ${PANEL_DIR}
  ${THIRD_PARTY_DIR}/uzlib/src
  ${GEN_DIR}
)

target_compile_definitions(app PRIVATE
  OD_APP_VERSION=0x0300
  # Every display path runs on od_epd from the drive packets (shared/core/od_display.h); there
  # is no panel library and no panel-id table on this target.
  OD_CAP_DRIVE_WRITE=1
  OD_CAP_SPI_RAW=1
  OD_CAP_READ_IDENT=1
  # The boot render runs on a work queue beside config writes on main: the config-write hold
  # and a save's check-then-write share a k_mutex (opendisplay_config_storage.c).
  OD_CONFIG_STORE_LOCKING=1
)

if(OD_EPD_SPI_REQUIRE_SPIM)
  target_compile_definitions(app PRIVATE OD_EPD_SPI_REQUIRE_SPIM=1)
endif()

# BOARD *is* set here -- find_package(Zephyr) has run. Only the EXTRA_CONF_FILE block above it
# sees an empty BOARD; do not move these down-stream checks up there.
#
# The nRF52840 needed its own arm. This was a two-way if, so xiao_ble fell into the else and
# compiled as the L15 board -- meaning early init drove P2.3/P2.5/P2.10, which
# are nRF54L15 RF-switch and boost-select pins that DO NOT EXIST on the nRF52840 (P0/P1 only).
# It was silent rather than fatal because the pin decoder rejects a port whose gpio node is
# not okay and returns false, so every one of those writes was a no-op. Silent-and-wrong is
# still wrong: the board got no init of its own.
if(CONFIG_OD_PLATFORM_NRF52840)
  target_compile_definitions(app PRIVATE
    OD_PLATFORM_NRF52840
    OD_BOARD_XIAO_NRF52840
    # The module's QSPI flash. The nRF54 boards have no flash wired to this path.
    OD_CAP_IMGSTORE=1
    OD_IMGSTORE_WRITE_ONCE_HEADER=1
    OD_PLATFORM_NAME=\"nRF52840\"
    OPENDISPLAY_BUILD_ID=\"nrf52840\"
    # READ_IDENT byte 3: one value per MCU family.
    OD_IDENT_TARGET=OD_IDENT_TARGET_NRF52840)
elseif(CONFIG_OD_PLATFORM_NRF54)
  target_compile_definitions(app PRIVATE
    OD_PLATFORM_NRF54
    OD_PLATFORM_NAME=\"nRF54\"
    OPENDISPLAY_BUILD_ID=\"nrf54\"
    OD_IDENT_TARGET=OD_IDENT_TARGET_NRF54)
  if(CONFIG_SOC_NRF54LM20A)
    target_compile_definitions(app PRIVATE OD_BOARD_NRF54LM20A)
  elseif(CONFIG_SOC_NRF54L15)
    target_compile_definitions(app PRIVATE OD_BOARD_NRF54L15)
  else()
    message(FATAL_ERROR "Unsupported nRF54 SoC")
  endif()
else()
  message(FATAL_ERROR "Unsupported Nordic platform: expected nRF52840 or nRF54L")
endif()

# Application verbosity is a build-profile property, independent of Zephyr subsystem levels.
# Both profiles use the same OpenDisplay record format and native raw-log transport.
if(OD_DEBUG_BUILD)
  target_compile_definitions(app PRIVATE OD_DEBUG_BUILD=1 OD_LOG_LEVEL=OD_LOG_DEBUG)
else()
  target_compile_definitions(app PRIVATE OD_LOG_LEVEL=OD_LOG_INFO)
endif()
target_compile_definitions(app PRIVATE OD_LOG_TEXT_MAX=253u OD_LOG_RAW_TEXT_MAX=255u)
if(od_bench_build)
  target_compile_definitions(app PRIVATE ${OD_BENCH_DEFINES})
  target_sources(app PRIVATE ${SRC_DIR}/od_test_bench.c)
endif()

# Default BLE TX power in dBm, used when the stored config leaves PowerOption.tx_power at 0.
#
# THIS IS THE ONLY BUILD-TIME KNOB THAT REACHES THE RADIO ON THIS TARGET. Zephyr's
# BT_CTLR_TX_PWR_* choice looks like the obvious one and is inert here: CONFIG_BT_LL_SOFTDEVICE
# selects the SoftDevice Controller, and BT_CTLR_TX_PWR_DBM has no consumer outside Zephyr's own
# software link layer (BT_LL_SW_SPLIT). Power is set at runtime instead, by the HCI VS
# Write_Tx_Power_Level that opendisplay_ble.c's apply_tx_power() issues -- so the default has to
# reach that call, not Kconfig.
#
# +8 dBm is the maximum on every board this target builds: nRF52840 sets
# HAS_HW_NRF_RADIO_TX_PWR_HIGH (radio-tx-high-power-supported in its DTS) and nRF54L15/LM20A are
# SOC_COMPATIBLE_NRF54LX. The controller clamps to the nearest supported step and reports what it
# took; apply_tx_power() logs requested and selected so the two are never assumed equal.
if(NOT DEFINED OD_TX_POWER_DBM)
  set(OD_TX_POWER_DBM 8)
endif()
target_compile_definitions(app PRIVATE OD_TX_POWER_DBM=${OD_TX_POWER_DBM})

# OpenDisplay app version string (boot screen + CMD_FIRMWARE_VERSION major.minor.patch).
# Prefer -DOD_FW_VERSION / -Dzephyr_OD_FW_VERSION; also accept ENV from build.sh
# because sysbuild does not always forward unprefixed cache vars into the app image.
set(_od_fw_ver "")
if(DEFINED OD_FW_VERSION AND NOT "${OD_FW_VERSION}" STREQUAL "")
  set(_od_fw_ver "${OD_FW_VERSION}")
elseif(DEFINED ENV{OD_FW_VERSION} AND NOT "$ENV{OD_FW_VERSION}" STREQUAL "")
  set(_od_fw_ver "$ENV{OD_FW_VERSION}")
elseif(DEFINED ENV{BUILD_VERSION} AND NOT "$ENV{BUILD_VERSION}" STREQUAL "")
  set(_od_fw_ver "$ENV{BUILD_VERSION}")
endif()
if(NOT "${_od_fw_ver}" STREQUAL "")
  target_compile_definitions(app PRIVATE OD_FW_VERSION=\"${_od_fw_ver}\")
  message(STATUS "OpenDisplay FW version: ${_od_fw_ver}")
endif()
# Prefer -DGIT_SHA / -Dzephyr_GIT_SHA (sysbuild strips zephyr_); also ENV{SHA}.
set(_od_git_sha "")
if(DEFINED GIT_SHA AND NOT "${GIT_SHA}" STREQUAL "")
  set(_od_git_sha "${GIT_SHA}")
elseif(DEFINED ENV{SHA} AND NOT "$ENV{SHA}" STREQUAL "")
  set(_od_git_sha "$ENV{SHA}")
endif()
if(NOT "${_od_git_sha}" STREQUAL "")
  target_compile_definitions(app PRIVATE SHA=\"${_od_git_sha}\")
endif()
if(FACTORY_CONFIG_HEX)
  target_compile_definitions(app PRIVATE FACTORY_HAS_EMBED=1)
endif()
if(FACTORY_CLEAR_CONFIG_ON_BOOT)
  target_compile_definitions(app PRIVATE FACTORY_CLEAR_CONFIG_ON_BOOT=1)
endif()
if(OD_LOW_POWER_QUIET)
  target_compile_definitions(app PRIVATE OD_LOW_POWER_QUIET=1)
endif()

target_sources(app PRIVATE
  ${SRC_DIR}/main.c
  ${SRC_DIR}/od_board_common.c
  ${SRC_DIR}/od_gpio.c
  ${SRC_DIR}/od_pin_codec.c
  ${SRC_DIR}/od_zephyr_time.c
  ${SRC_DIR}/od_hal_log.c
  ${SRC_DIR}/od_hal_nvs.c
  ${SRC_DIR}/od_hal_i2c.c
  ${SRC_DIR}/od_sensor_app.c
  ${SRC_DIR}/od_hal_wdt.c
  ${SRC_DIR}/od_hal_crypto.c
  ${SRC_DIR}/od_watchdog_app.c
  ${SRC_DIR}/opendisplay_idle_wake.c
  ${SRC_DIR}/opendisplay_ble.c
  ${SRC_DIR}/opendisplay_battery.c
  ${SRC_DIR}/opendisplay_i2c.c
  ${SRC_DIR}/opendisplay_sensor_npm1300.c
  ${SRC_DIR}/opendisplay_button.c
  ${SRC_DIR}/opendisplay_config_parser.c
  ${SRC_DIR}/opendisplay_config_storage.c
  ${SRC_DIR}/opendisplay_display.c
  ${SRC_DIR}/od_epd_spi_nrfx.c
  ${SRC_DIR}/od_epd_nordic.c
  ${SRC_DIR}/od_inflate_app.c
  ${SRC_DIR}/opendisplay_led.c
  ${SRC_DIR}/od_touch_app.c
  ${SRC_DIR}/opendisplay_buzzer.c
  ${SRC_DIR}/opendisplay_pipe.c
  ${SRC_DIR}/od_cmd_device.c
  ${SRC_DIR}/od_cmd_config.c
  ${SRC_DIR}/od_rxq_app.c
  ${SRC_DIR}/od_sec2_app.c
  ${SRC_DIR}/od_health_app.c
  ${SRC_DIR}/od_claim_app_nordic.c
  ${SRC_DIR}/od_hal_secret.c
  ${SRC_DIR}/od_hal_radio.c
  ${SRC_DIR}/od_cmd_reply.c
  ${SRC_DIR}/opendisplay_cs.c
  ${SRC_DIR}/opendisplay_nfc.c
  ${SRC_DIR}/factory_config.c
  ${GEN_DIR}/factory_config_data.c
  ${THIRD_PARTY_DIR}/qrcode/qrcode.c
)

if(NOT OD_EPD_SPI_REQUIRE_SPIM)
  target_sources(app PRIVATE ${SRC_DIR}/od_epd_spi_bitbang.c)
endif()

# ------------------------------------------------------------------------------ shared/ ---
# PURE needs the C standard library and shared/protocol, nothing else. HAL_WDT needs
# shared/hal/od_hal_wdt.h, which src/od_hal_wdt.c implements over the devicetree watchdog0 and
# gpregret2 nodes.
#
# HAL_ADV joined here (targets/nordic-zephyr/src/opendisplay_ble.c implements
# od_hal_adv_program/start/stop() at the bottom of that file -- see its own header comment for
# why it lives there, not in a separate hal/od_hal_adv.c). Fixes the confirmed-on-hardware
# advertising-does-not-resume-after-disconnect bug: disconnect handling now reconciles through
# od_adv_control every opendisplay_ble_process() pass instead of a blind 150ms k_work_delayable.
# APP_RXQ joins at C9: the inbound ring is shared, and this target implements its two seam
# predicates in src/od_rxq_app.c. It needs no HAL -- the ring logs its own arrivals and drops.
# C10 adds HAL_RADIO and APP_SESSION: this target now implements od_hal_radio (src/od_hal_radio.c)
# and the od_sec2_app seam (src/od_sec2_app.c), so it takes the shared egress, gate,
# dispatcher and config-read producer rather than its own copies.
target_sources(app PRIVATE ${OD_SHARED_SOURCES_PURE} ${OD_SHARED_SOURCES_HAL_CRYPTO}
                           ${OD_SHARED_SOURCES_HAL_NVS}
                           ${OD_SHARED_SOURCES_HAL_WDT} ${OD_SHARED_SOURCES_APP_RXQ}
                           ${OD_SHARED_SOURCES_HAL_RADIO} ${OD_SHARED_SOURCES_APP_SESSION}
                           ${OD_SHARED_SOURCES_HAL_ADV} ${OD_SHARED_SOURCES_APP_BOOT}
                           ${OD_SHARED_SOURCES_APP_INFLATE} ${OD_SHARED_SOURCES_APP_XFER}
                           ${OD_SHARED_SOURCES_APP_NFC} ${OD_SHARED_SOURCES_APP_LED}
                           ${OD_SHARED_SOURCES_APP_BUZZER}
                           ${OD_SHARED_SOURCES_APP_SENSOR}
                           ${OD_SHARED_SOURCES_APP_TOUCH}
                           ${OD_SHARED_SOURCES_APP_EPD}
                           ${OD_SHARED_SOURCES_APP_DRIVE}
                           ${OD_SHARED_SOURCES_APP_DISPLAY}
                           ${OD_SHARED_SOURCES_APP_IDENT}
                           ${OD_SHARED_SOURCES_APP_SPI_RAW}
                           ${OD_SHARED_SOURCES_HAL_LOG})

# The per-source INCLUDE_DIRECTORIES property that used to sit here is GONE, and its removal is
# the point of the header adoption: it existed only to pin these two shared TUs to the canonical
# opendisplay_protocol.h / opendisplay_structs.h while ${SRC_DIR} still carried subsets of the
# same names. With the subsets deleted there is one definition of each on the include path, so
# the whole target -- shared and local alike -- compiles against the same wire contract, which is
# what the ordering above now enforces for everyone rather than for two files.

if(CONFIG_OD_PLATFORM_NRF52840)
  target_sources(app PRIVATE
    ${SRC_DIR}/platform/nrf52840/od_board_nrf52840.c
    ${SRC_DIR}/platform/nrf52840/od_pin_codec_nrf52840.c
    ${SRC_DIR}/od_hal_spi.c
    ${SRC_DIR}/od_cmd_flash.c
    ${OD_SHARED_SOURCES_APP_IMGSTORE})
elseif(CONFIG_SOC_NRF54LM20A)
  target_sources(app PRIVATE
    ${SRC_DIR}/platform/nrf54/od_board_nrf54lm20a.c
    ${SRC_DIR}/platform/nrf54/od_pin_codec_nrf54.c)
elseif(CONFIG_SOC_NRF54L15)
  target_sources(app PRIVATE
    ${SRC_DIR}/platform/nrf54/od_board_nrf54l15.c
    ${SRC_DIR}/platform/nrf54/od_pin_codec_nrf54.c)
endif()

add_dependencies(app factory_config_gen)
