# Host build of shared/ + the test harness.
#
# This is the real enforcement of the shared/ boundary. .github/workflows/shared-boundary.yml
# is a grep: it sees #include lines and literal tokens, and structurally cannot see an
# extern-declared vendor symbol, a macro leaked through a target config header, or a libc
# call that does not exist on a freestanding target. Compiling shared/ for the host with a
# strict, warning-clean C99 toolchain does. See docs/DESIGN_REVIEW_2026-07-25.md § F4/F8.
#
#   cmake -S tests/host -B build && cmake --build build && ctest --test-dir build
#
# Green against an empty shared/ by design (docs/TEST_OWNERSHIP.md § "First step").

cmake_minimum_required(VERSION 3.16)
# CXX is here for ONE reason: two Nordic target sources are .cpp, and the suites that cover them
# compile the production file rather than a transcription of it. shared/ remains plain C and is
# still built by the C compiler -- the boundary checks in tools/check.sh enforce that separately.
project(od_host_tests C CXX)

enable_testing()
find_package(Threads REQUIRED)

# -----------------------------------------------------------------------------------
# The contract: C99, no compiler extensions, warnings are errors.
# -----------------------------------------------------------------------------------
# No GNU extensions: shared/ must build under three unrelated vendor toolchains, and a
# GNU-ism that gcc accepts here is a failure discovered later on a target instead.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -Werror)

# Both gcc and clang are supported and CI runs both; neither is required locally. Select
# with -DCMAKE_C_COMPILER=clang. Anything else builds too, it is just not what CI gates on.
message(STATUS "od: C compiler is ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")

# -----------------------------------------------------------------------------------
# shared/ — one source list, shared with the three target builds.
# -----------------------------------------------------------------------------------
include(${CMAKE_CURRENT_LIST_DIR}/../../shared/sources.cmake)

if(OD_SHARED_SOURCES)
    add_library(od_shared STATIC ${OD_SHARED_SOURCES})
    target_include_directories(od_shared PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    # BG22 changes the ABI of cap-sensitive shared structs and the PIPE reservation table. Compile
    # a real second variant; executable-local definitions cannot retroactively change od_shared.
    add_library(od_shared_silabs STATIC ${OD_SHARED_SOURCES})
    target_include_directories(od_shared_silabs PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    # The profile comes from shared/profiles.cmake, the same list the firmware compiles with;
    # OD_BOOT_LOGO_SIZES is host-only, because BG22 declines APP_BOOT and never builds the logo.
    target_compile_definitions(od_shared_silabs PUBLIC
        ${OD_PROFILE_SILABS}
        OD_BOOT_LOGO_SIZES=2)

    # The classic ESP32 profile negotiates a 16-frame PIPE window. Compile the complete shared
    # archive under that geometry so the derived reorder storage and queue assertions are tested,
    # rather than changing only the test translation unit after the archive was built.
    add_library(od_shared_pipe_w16 STATIC ${OD_SHARED_SOURCES})
    target_include_directories(od_shared_pipe_w16 PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    target_compile_definitions(od_shared_pipe_w16 PUBLIC
        OD_PIPE_MAX_W=16u
        OD_PIPE_MAX_N=16u)

    # Per-frame transfer progress is DEBUG-only; completion remains visible at INFO. Compile the
    # three state-owning translation units at that level so xfer_test exercises the code that
    # ships in debug target profiles. Executable-local definitions cannot alter objects already
    # compiled into od_shared.
    add_library(od_xfer_log_debug STATIC
        ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_xfer.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_xfer_direct.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_xfer_partial.c)
    target_include_directories(od_xfer_log_debug PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    target_compile_definitions(od_xfer_log_debug PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)

    # PIPE owns its negotiation record and INFO summary counters. Compile that translation unit
    # at DEBUG separately; od_xfer_state_t is log-level invariant, so it safely consumes the
    # ordinary INFO transfer archive while exercising the deployed DEBUG branch.
    add_library(od_pipe_log_debug STATIC
        ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_pipe.c)
    target_include_directories(od_pipe_log_debug PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    target_compile_definitions(od_pipe_log_debug PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)

    # The parsed config dump is DEBUG-only. Compile both state-owning translation units at that
    # level so the debug fixture tests the code that target debug profiles actually include.
    add_library(od_config_log_debug STATIC
        ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_config.c
        ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_config_log.c)
    target_include_directories(od_config_log_debug PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    target_compile_definitions(od_config_log_debug PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)

    # Deferral records are DEBUG-only and carry episode-suppression state in od_dispatch.c. Compile
    # that translation unit at the deployed debug level so the ordering fixture exercises both.
    add_library(od_dispatch_log_debug STATIC
        ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_dispatch.c)
    target_include_directories(od_dispatch_log_debug PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    target_compile_definitions(od_dispatch_log_debug PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)

    # Dispatch-only fixtures define the four promoted od_xfer command entry points so they can
    # observe routing and scripted verdicts. Keep production od_xfer objects out of these archives;
    # otherwise any unrelated state query pulls them in and collides with the fixture definitions.
    # od_core is excluded too because reset composition owns transfer lifecycle, not dispatch tests.
    set(OD_SHARED_DISPATCH_FIXTURE_SOURCES
        ${OD_SHARED_SOURCES_PURE}
        ${OD_SHARED_SOURCES_HAL_CRYPTO}
        ${OD_SHARED_SOURCES_HAL_RADIO}
        ${OD_SHARED_SOURCES_APP_SESSION})
    list(FILTER OD_SHARED_DISPATCH_FIXTURE_SOURCES EXCLUDE REGEX "/core/od_core\\.c$")
    add_library(od_shared_dispatch_fixture STATIC ${OD_SHARED_DISPATCH_FIXTURE_SOURCES})
    target_include_directories(od_shared_dispatch_fixture PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    add_library(od_shared_dispatch_fixture_silabs STATIC ${OD_SHARED_DISPATCH_FIXTURE_SOURCES})
    target_include_directories(od_shared_dispatch_fixture_silabs PUBLIC ${OD_SHARED_INCLUDE_DIRS})
    target_compile_definitions(od_shared_dispatch_fixture_silabs PUBLIC
        ${OD_PROFILE_SILABS}
        OD_BOOT_LOGO_SIZES=2)
    message(STATUS "od: shared/ has ${CMAKE_CURRENT_LIST_DIR} sources — building od_shared")
else()
    # No target code is imported yet. An INTERFACE library keeps every consumer below
    # writing `target_link_libraries(... od_shared)` unchanged, so the first real source
    # to land flips the branch above and nothing else in this file moves.
    add_library(od_shared INTERFACE)
    target_include_directories(od_shared INTERFACE ${OD_SHARED_INCLUDE_DIRS})
    message(STATUS "od: shared/ source list is empty (expected — scaffold; see shared/sources.cmake)")
endif()

# -----------------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------------
# canary: compiles the synced protocol headers under the flags above and checks a few
# wire invariants at compile time. Its job is to make this build non-vacuous while
# shared/ is empty — delete it once there are real sources. See canary.c.
add_executable(od_canary canary.c)
target_link_libraries(od_canary od_shared)
add_test(NAME canary COMMAND od_canary)

# span: od_span_t itself. Header-only, so nothing else in this file would compile it on its own
# terms -- and it is now the single place the config walk's "does this packet fit" arithmetic is
# written, after fifteen copies of it on the pre-auth surface were collapsed into one. A defect
# in od_span_split() is a defect in every caller simultaneously, which is precisely the shape of
# bug that a suite testing only the callers can miss.
add_executable(od_span_test span_test.c)
target_link_libraries(od_span_test od_shared)
add_test(NAME span COMMAND od_span_test)

add_executable(od_boot_payload_test boot_payload_test.c)
target_link_libraries(od_boot_payload_test od_shared)
add_test(NAME boot_payload COMMAND od_boot_payload_test)

add_executable(od_color_test color_test.c)
target_link_libraries(od_color_test od_shared)
add_test(NAME color COMMAND od_color_test)

add_executable(od_boot_screen_test boot_screen_test.c
    ${CMAKE_CURRENT_LIST_DIR}/../../third_party/qrcode/qrcode.c)
target_link_libraries(od_boot_screen_test od_shared)
add_test(NAME boot_screen COMMAND od_boot_screen_test)

# zlib_inflate: the portable streaming inflater. C14.2 characterized its former heap and static
# storage modes with the same suite; the results were identical, so C14.4 retired heap selection
# and this permanent test now covers the one static implementation.
add_executable(od_zlib_inflate_test zlib_inflate_test.c)
target_link_libraries(od_zlib_inflate_test od_shared)
add_test(NAME zlib_inflate COMMAND od_zlib_inflate_test)

# The shared header rule both inflate engines apply. Its own suite because the ESP32 adapter
# cannot be linked here, so the rule is what carries the cross-engine claim.
add_executable(od_zlib_header_test zlib_header_test.c)
target_link_libraries(od_zlib_header_test od_shared)
add_test(NAME zlib_header COMMAND od_zlib_header_test)

# The vendored QR encoder. boot_screen_test compiles it too, but only through a rendered page --
# this pins the module bitmap itself so a re-vendor is a failing test, not a QR that scans
# differently on a shipped device.
add_executable(od_qrcode_test qrcode_test.c
    ${CMAKE_CURRENT_LIST_DIR}/../../third_party/qrcode/qrcode.c)
target_include_directories(od_qrcode_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../third_party/qrcode)
add_test(NAME qrcode COMMAND od_qrcode_test)

add_executable(od_zlib_pump_test zlib_pump_test.c)
target_link_libraries(od_zlib_pump_test od_shared)
add_test(NAME zlib_pump COMMAND od_zlib_pump_test)

add_executable(od_xfer_test xfer_test.c)
target_compile_definitions(od_xfer_test PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)
target_link_libraries(od_xfer_test od_xfer_log_debug od_shared)
add_test(NAME xfer COMMAND od_xfer_test)

# The completion summary is INFO even though frame heads and progress are DEBUG. Build the same
# behavioral suite against the ordinary shared archive to pin that shipping-profile distinction.
add_executable(od_xfer_info_test xfer_test.c)
target_link_libraries(od_xfer_info_test od_shared)
add_test(NAME xfer_info COMMAND od_xfer_info_test)

add_executable(od_xfer_silabs_test xfer_silabs_test.c)
target_link_libraries(od_xfer_silabs_test od_shared_silabs)
add_test(NAME xfer_silabs COMMAND od_xfer_silabs_test)

add_library(od_xfer_auto STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_xfer.c
    ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_xfer_direct.c
    ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_xfer_partial.c)
target_include_directories(od_xfer_auto PUBLIC ${OD_SHARED_INCLUDE_DIRS})
target_compile_definitions(od_xfer_auto PRIVATE OD_XFER_DIRECT_AUTO_END=1)
add_executable(od_xfer_auto_test xfer_auto_test.c od_log_test_stub.c)
target_link_libraries(od_xfer_auto_test od_xfer_auto od_shared)
add_test(NAME xfer_auto COMMAND od_xfer_auto_test)

add_executable(od_xfer_reply_session_test xfer_reply_session_test.c od_log_test_stub.c)
target_link_libraries(od_xfer_reply_session_test od_session_fake)
add_test(NAME xfer_reply_session COMMAND od_xfer_reply_session_test)

add_executable(od_pipe_test pipe_test.c)
target_link_libraries(od_pipe_test od_shared)
add_test(NAME pipe COMMAND od_pipe_test)

add_executable(od_pipe_debug_test pipe_test.c)
target_compile_definitions(od_pipe_debug_test PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)
target_link_libraries(od_pipe_debug_test od_pipe_log_debug od_shared)
add_test(NAME pipe_debug COMMAND od_pipe_debug_test)

add_executable(od_pipe_w16_test pipe_test.c)
target_link_libraries(od_pipe_w16_test od_shared_pipe_w16)
add_test(NAME pipe_w16 COMMAND od_pipe_w16_test)

add_executable(od_pipe_off_test
    pipe_off_test.c od_xfer_app_test_stub.c)
target_link_libraries(od_pipe_off_test od_shared_silabs)
add_test(NAME pipe_off COMMAND od_pipe_off_test)

# adv_control: the loop-owned advertising policy machine (correctness review F4) driven against
# a scripted fake HAL. The fake supplies od_hal_adv_{program,start,stop} at LINK time, exactly
# as a target does — shared/ binds its HAL at link time, so there is no injection seam to fake
# around and the test exercises the real linkage.
#
# What this cannot prove, stated so a green run is not over-quoted: AD-record bytes, that
# NimBLE accepts the call sequence, and that a target's callback-to-loop bridge publishes
# coherently. Those need the Milestone 0 byte fixture and a board.
add_executable(od_adv_control_test adv_control_test.c)
target_link_libraries(od_adv_control_test od_shared)
add_test(NAME adv_control COMMAND od_adv_control_test)

# config_asm: chunked CONFIG_WRITE reassembly (correctness review F3). Pure state machine over
# a byte stream, so the host test IS the coverage -- there is no target-specific behaviour to
# leave unverified here, unlike the advertising controller.
#
# Every rejection case asserts the result is not COMPLETE, which is the same assertion as
# "storage did not change": COMPLETE is the only result that lets a caller write NVS.
add_executable(od_config_asm_test config_asm_test.c)
target_link_libraries(od_config_asm_test od_shared)
add_test(NAME config_asm COMMAND od_config_asm_test)

add_executable(od_config_asm_cap_test config_asm_cap_test.c)
target_link_libraries(od_config_asm_cap_test od_shared_silabs)
add_test(NAME config_asm_cap COMMAND od_config_asm_cap_test)

# config_store: the stored record's framing, CRC and bounds over a fake MEDIUM -- one that holds
# bytes and injects failures, and knows nothing about magic, versions or checksums. The record
# shape is asserted as literal little-endian bytes rather than by re-encoding it, so a change
# made in both the writer and the reader still fails here.
#
# Built against both caps, because OD_CONFIG_MAX_SIZE is the one thing that differs per target
# and it decides what is refused -- and because it changes the ABI of the structs this module
# static-asserts against (CLAUDE.md decision 12).
add_executable(od_config_store_test config_store_test.c)
target_link_libraries(od_config_store_test od_shared)
add_test(NAME config_store COMMAND od_config_store_test)

add_executable(od_config_store_cap_test config_store_test.c)
target_link_libraries(od_config_store_cap_test od_shared_silabs)
add_test(NAME config_store_cap COMMAND od_config_store_cap_test)

# i2c_contract: shared/hal/od_hal_i2c.h as an EXECUTABLE contract. The assertions are about
# framing and bus selection -- where STOPs fall, and which pins an operation reaches -- because
# those are what an engine gets wrong while still returning plausible bytes. Bound here to a
# reference engine; the per-target adapters bind to the same body as their cutovers land
# (PLAN_SENSOR_SEAM_2026-08-23.md staging steps 4, 5 and 9).
add_executable(od_i2c_contract_ref_test
    i2c_contract_ref_test.c
    od_log_test_stub.c
    fake_i2c/i2c_wire.c
    fake_i2c/i2c_ref_impl.c)
target_include_directories(od_i2c_contract_ref_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(od_i2c_contract_ref_test od_shared)
add_test(NAME i2c_contract_ref COMMAND od_i2c_contract_ref_test)

# silabs_i2c_trace: the BG22 TNB132M transport at the EDGES. Mandatory per
# PLAN_SENSOR_SEAM_2026-08-23.md staging step 9b, and NOT covered by the existing BG22 NFC tests,
# which fake od_nfc_app_read/write -- above the transport -- and so say nothing about START
# placement, ACK sampling or the NACK on a final read byte. Binds the production hal/od_hal_i2c.c
# to fake GPIO and delay. It must never assert stuck-bus behaviour: that engine reads SDA only.
add_executable(od_silabs_i2c_trace_test
    silabs_i2c_trace_test.c
    fake_silabs/em_gpio.c
    ../../targets/efr32bg22-slc/hal/od_hal_i2c.c)
target_include_directories(od_silabs_i2c_trace_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_silabs
    ../../targets/efr32bg22-slc)
target_link_libraries(od_silabs_i2c_trace_test od_shared_silabs)
add_test(NAME silabs_i2c_trace COMMAND od_silabs_i2c_trace_test)

# nordic_i2c_trace: the PREREQUISITE the plan gained after step 4's review. An adapter that hands
# pins to an engine can transpose SCL and SDA, and that compiles, links and passes everything else
# here -- it is exactly what shipped in b5bdffe. The trace is labelled by pin CONFIG BYTE, the two
# the DataBus record carries, so a swap moves every clock edge onto the data line and fails.
# Binds the production od_hal_i2c.c and opendisplay_i2c.c.
add_executable(od_nordic_i2c_trace_test
    nordic_i2c_trace_test.c
    od_log_test_stub.c
    fake_nordic_i2c/od_gpio_trace.c
    ../../targets/nordic-zephyr/src/od_hal_i2c.c
    ../../targets/nordic-zephyr/src/opendisplay_i2c.c)
target_include_directories(od_nordic_i2c_trace_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_nordic_i2c
    ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr
    ../../targets/nordic-zephyr/src)
target_link_libraries(od_nordic_i2c_trace_test od_shared)
add_test(NAME nordic_i2c_trace COMMAND od_nordic_i2c_trace_test)

# sensor_sht40: the shared driver's DEVICE POLICY over the fake wire -- reset and fallback
# addresses, the write/delay/read ordering (two transactions, not a repeated START), both CRC-8
# words independently, conversion and clamping, the poll TTL, the invalid-sample marker, and the
# two-pass retry the Nordic port had dropped.
add_executable(od_sensor_sht40_test
    sensor_sht40_test.c
    od_log_test_stub.c
    fake_i2c/i2c_wire.c
    fake_i2c/i2c_ref_impl.c)
target_include_directories(od_sensor_sht40_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(od_sensor_sht40_test od_shared)
add_test(NAME sensor_sht40 COMMAND od_sensor_sht40_test)

# sensor_bq27220: first-match-only walk, two-byte voltage / one-byte SOC widths, the repeated
# START each register read needs, the charging TRI-STATE (unknown is not "not charging"), cache
# invalidation on a failed read, the SOC clamp and the TTL across a 32-bit wrap.
add_executable(od_sensor_bq27220_test
    sensor_bq27220_test.c
    od_log_test_stub.c
    fake_i2c/i2c_wire.c
    fake_i2c/i2c_ref_impl.c)
target_include_directories(od_sensor_bq27220_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(od_sensor_bq27220_test od_shared)
add_test(NAME sensor_bq27220 COMMAND od_sensor_bq27220_test)

# touch_gt911: the shared driver against a register-addressed fake part. The MSD expectations were
# written from the donor's format comment BEFORE the packing code, because that comment is the only
# normative description of the layout that exists and three independent hosts implement it.
#
# The map cases are the ones worth reading, and the obvious probe does NOT work: a driver that
# caches raw and maps at pack time re-maps the cached value to the same pixel, so the latched
# release agrees byte for byte. What separates them is change detection -- two raw coordinates that
# CLIP to the same pixel are one sample with the map before the cache and two after it.
# No od_log_test_stub.c: this suite defines _od_log itself so it can COUNT records -- an
# unsupported touch_ic_type must be diagnosed once, not once per service pass.
add_executable(od_touch_gt911_test
    touch_gt911_test.c)
target_include_directories(od_touch_gt911_test PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(od_touch_gt911_test od_shared)
add_test(NAME touch_gt911 COMMAND od_touch_gt911_test)

# silabs_storage: the BG22 overlay, where od_config_asm's reassembly state and the NVM3 record are
# one object. The two static asserts in the storage file compare LAYOUT; this compares SEQUENCING
# -- the header is written over the four live assembler state words, so the order of capture,
# write and reset is the whole correctness argument and no offsetof can check it. Production
# opendisplay_config_storage.c over a fake NVM3; no Python needed, unlike the corpus profiles.
add_executable(od_silabs_storage_test
    silabs_storage_test.c
    fake_silabs/nvm3_fake.c
    ../../targets/efr32bg22-slc/od_hal_nvs.c
    ../../targets/efr32bg22-slc/opendisplay_config_storage.c)
target_include_directories(od_silabs_storage_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_silabs
    ../../targets/efr32bg22-slc)
target_link_libraries(od_silabs_storage_test od_shared_silabs)
add_test(NAME silabs_storage COMMAND od_silabs_storage_test)

# time_silabs: compile the production BG22 adapter over fake sleeptimer/udelay primitives. The
# load-bearing case crosses the underlying 32-bit tick rollover and proves conversion happens in
# 64 bits before the public millisecond result is narrowed.
add_executable(od_time_silabs_test
    time_silabs_test.c
    ../../targets/efr32bg22-slc/od_hal_time.c)
target_include_directories(od_time_silabs_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_silabs)
target_link_libraries(od_time_silabs_test od_shared_silabs)
add_test(NAME time_silabs COMMAND od_time_silabs_test)

# logging: the same shared implementation under the two deployed truncation profiles, plus a
# capability-off build that deliberately supplies no log HAL. Compile the production source
# directly so each executable gets exactly one profile rather than the aggregate archive's default.
set(OD_LOG_SHARED_SOURCE ${CMAKE_CURRENT_LIST_DIR}/../../shared/core/od_log.c)
add_executable(od_log_esp_test log_test.c ${OD_LOG_SHARED_SOURCE})
target_include_directories(od_log_esp_test PRIVATE ${OD_SHARED_INCLUDE_DIRS})
target_compile_definitions(od_log_esp_test PRIVATE
    OD_LOG_TEXT_MAX=232u OD_LOG_RAW_TEXT_MAX=232u)
target_link_libraries(od_log_esp_test Threads::Threads)
add_test(NAME log_esp COMMAND od_log_esp_test)

add_executable(od_log_nordic_test log_test.c ${OD_LOG_SHARED_SOURCE})
target_include_directories(od_log_nordic_test PRIVATE ${OD_SHARED_INCLUDE_DIRS})
target_compile_definitions(od_log_nordic_test PRIVATE
    OD_LOG_TEXT_MAX=253u OD_LOG_RAW_TEXT_MAX=255u)
target_link_libraries(od_log_nordic_test Threads::Threads)
add_test(NAME log_nordic COMMAND od_log_nordic_test)

add_executable(od_log_off_test log_off_test.c ${OD_LOG_SHARED_SOURCE})
target_include_directories(od_log_off_test PRIVATE ${OD_SHARED_INCLUDE_DIRS})
target_compile_definitions(od_log_off_test PRIVATE OD_CAP_LOG=0)
target_compile_options(od_log_off_test PRIVATE -O0)
add_test(NAME log_off COMMAND od_log_off_test)

add_executable(od_nordic_log_hal_test
    nordic_log_hal_test.c
    ../../targets/nordic-zephyr/src/od_hal_log.c)
target_include_directories(od_nordic_log_hal_test PRIVATE
    ${OD_SHARED_INCLUDE_DIRS}
    ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr)
# The record bounds the Nordic firmware is built with, so the terminator rewrite is exercised at
# the sizes it actually sees (targets/nordic-zephyr/zephyr/CMakeLists.txt).
target_compile_definitions(od_nordic_log_hal_test PRIVATE
    OD_LOG_TEXT_MAX=253u OD_LOG_RAW_TEXT_MAX=255u)
add_test(NAME nordic_log_hal COMMAND od_nordic_log_hal_test)

# Nordic EPD GPIO fallback: compile the production direct-register writer over a fake nRF GPIO
# register surface. This pins the one-time decode contract and the mode-0 edge/data ordering;
# transfer-rate qualification still requires a board and oscilloscope.
add_executable(od_nordic_epd_bitbang_test
    nordic_epd_bitbang_test.c
    ../../targets/nordic-zephyr/src/od_epd_spi_bitbang.c)
target_include_directories(od_nordic_epd_bitbang_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_nrf_gpio
    ../../targets/nordic-zephyr/src)
add_test(NAME nordic_epd_bitbang COMMAND od_nordic_epd_bitbang_test)

# Compile each production board policy independently. The exact nRF54 signal tuples are a
# package-routing contract; a port-level approximation would incorrectly admit cross-pairs.
foreach(OD_EPD_POLICY IN ITEMS NRF52840 NRF54L15 NRF54LM20A)
    string(TOLOWER ${OD_EPD_POLICY} OD_EPD_POLICY_LOWER)
    if(OD_EPD_POLICY STREQUAL "NRF52840")
        set(OD_EPD_POLICY_SOURCE ../../targets/nordic-zephyr/src/platform/nrf52840/od_board_nrf52840.c)
        set(OD_EPD_POLICY_DEFINE CONFIG_OD_PLATFORM_NRF52840=1)
    elseif(OD_EPD_POLICY STREQUAL "NRF54L15")
        set(OD_EPD_POLICY_SOURCE ../../targets/nordic-zephyr/src/platform/nrf54/od_board_nrf54l15.c)
        set(OD_EPD_POLICY_DEFINE CONFIG_SOC_NRF54L15=1)
    else()
        set(OD_EPD_POLICY_SOURCE ../../targets/nordic-zephyr/src/platform/nrf54/od_board_nrf54lm20a.c)
        set(OD_EPD_POLICY_DEFINE CONFIG_SOC_NRF54LM20A=1)
    endif()
    add_executable(od_nordic_epd_policy_${OD_EPD_POLICY_LOWER}_test
        nordic_epd_policy_test.c ${OD_EPD_POLICY_SOURCE})
    target_include_directories(od_nordic_epd_policy_${OD_EPD_POLICY_LOWER}_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr
        ../../targets/nordic-zephyr/src)
    target_compile_definitions(od_nordic_epd_policy_${OD_EPD_POLICY_LOWER}_test PRIVATE
        ${OD_EPD_POLICY_DEFINE} OD_TEST_POLICY_${OD_EPD_POLICY}=1)
    add_test(NAME nordic_epd_policy_${OD_EPD_POLICY_LOWER}
        COMMAND od_nordic_epd_policy_${OD_EPD_POLICY_LOWER}_test)
endforeach()

# Production SPIM backend over deterministic nrfx/IRQ/semaphore fakes. Build all three compile-time
# instance selections, plus the strict qualification variant that has no bit-bang references.
function(add_nordic_epd_spim_test suffix platform_define)
    set(target od_nordic_epd_spim_${suffix}_test)
    add_executable(${target}
        nordic_epd_spim_test.c
        fake_nordic_spim/nrfx_fake.c
        ../../targets/nordic-zephyr/src/od_epd_spi_nrfx.c)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fake_nordic_spim
        ../../targets/nordic-zephyr/src
        ../../targets/nordic-zephyr/panel
        ../../shared/core)
    target_compile_definitions(${target} PRIVATE
        OD_EPD_SPI_HOST_TEST=1 ${platform_define} ${ARGN})
    add_test(NAME nordic_epd_spim_${suffix} COMMAND ${target})
endfunction()

add_nordic_epd_spim_test(nrf52840 CONFIG_OD_PLATFORM_NRF52840=1)
add_nordic_epd_spim_test(nrf54l15 CONFIG_SOC_NRF54L15=1)
add_nordic_epd_spim_test(nrf54lm20a CONFIG_SOC_NRF54LM20A=1)
add_nordic_epd_spim_test(nrf52840_strict CONFIG_OD_PLATFORM_NRF52840=1
    OD_EPD_SPI_REQUIRE_SPIM=1)

# config_tlv: the config-blob walk and its CRC. The walk is the PRE-AUTH attack surface -- this
# blob is parsed at boot from storage a client can write -- so the cases that matter are the
# hostile ones, and the CRC is checked DIFFERENTIALLY against an independent transcription of
# the shipped algorithm rather than against constants derived from the code under test.
add_executable(od_config_tlv_test config_tlv_test.c)
target_link_libraries(od_config_tlv_test od_shared)
add_test(NAME config_tlv COMMAND od_config_tlv_test)

# config: the parsed aggregate -- instance caps, the Firmware zero-key rule, string termination,
# and what a truncated blob leaves behind. Downstream of the pre-auth surface od_config_tlv
# guards, so the cases are the ones where a hostile blob still reaches storage.
#
# The zero-key case is the one to keep: Nordic and Silabs do not normalise it today, so this is
# the assertion that says the promotion settled the divergence rather than averaging it.
add_executable(od_config_test config_test.c)
target_link_libraries(od_config_test od_shared)
add_test(NAME config COMMAND od_config_test)

add_executable(od_config_debug_test config_test.c)
target_compile_definitions(od_config_debug_test PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)
target_link_libraries(od_config_debug_test od_config_log_debug od_shared)
add_test(NAME config_debug COMMAND od_config_debug_test)

# txq: the response queue, its capacity reservation and the drain, against a scripted radio.
#
# The cases that carry the weight are the ordering ones: RETRY must STOP a pass rather than skip
# past the entry (skipping reorders the wire), GONE must drop a whole tag while preserving the
# order of the survivors, and ERROR must retire exactly one entry -- retrying a permanent refusal
# is how a drain becomes a spin. Plus the reservation property the token exists for: one dispatch
# cannot spend another dispatch's capacity.
# od_txq.c logs its own drop line now; the stub keeps od_log.c out of the link. The suite's
# assertions still run through od_txq_app_dropped(), which stays live for BG22's sake.
add_executable(od_txq_test txq_test.c od_log_test_stub.c)
target_link_libraries(od_txq_test od_shared)
add_test(NAME txq COMMAND od_txq_test)

# DEBUG, deliberately, and od_rxq.c/od_log.c are compiled INTO the executable rather than taken
# from od_shared -- that archive is built at the default INFO, and a define applied only to the
# test TU would leave the code under test compiled at the other level (CLAUDE.md decision 12).
# It matters here because od_rxq.c's arrival line and both od_rxq_app.h predicates sit behind
# OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG: at INFO this suite would assert nothing about either
# and would link with the seam unimplemented. This is also the link proof that an APP_RXQ
# consumer supplies both functions.
add_executable(od_rxq_test rxq_test.c
    ../../shared/core/od_rxq.c
    ../../shared/core/od_log.c)
target_compile_definitions(od_rxq_test PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)
target_include_directories(od_rxq_test PRIVATE ${OD_SHARED_INCLUDE_DIRS})
target_link_libraries(od_rxq_test Threads::Threads)
add_test(NAME rxq COMMAND od_rxq_test)

# reply: the seal-or-plain decision, against a fake radio AND a fake od_session_app seam.
#
# The property is not "does it encrypt" but "is confidentiality chosen by the CALL". Both targets
# used to infer it from the response bytes, and Nordic sealed its own rejection frames -- which the
# host then decrypted and validated as an ACK for a command the device refused. The load-bearing
# cases are the ones where the bytes would mislead: a {0x00,cmd,0xFF} rejection, a TLS-LAN frame
# that must not be double-wrapped, and a live session with no security configured.
add_executable(od_reply_test reply_test.c)
target_link_libraries(od_reply_test od_session_fake)
add_test(NAME reply COMMAND od_reply_test)

# gate: one case per od_session result, checking the WIRE BYTES and the outcome.
#
# The compiler already proves the switches are exhaustive (od_gate.h explains the -Wswitch ratchet,
# verified by deleting a case and watching the build fail). What it cannot prove is that each
# member maps to the right action, which is where the consequences are: 0xFE advances the link's
# auth-abuse run and 0xFF does not, and a replayed PIPE frame must draw SILENCE because the 0x81
# NACK is fatal to the client's upload loop.
add_executable(od_gate_test gate_test.c)
target_link_libraries(od_gate_test od_session_fake)
add_test(NAME gate COMMAND od_gate_test)

# config_read: CONFIG_READ as a resumable producer, driven against a queue that keeps filling.
#
# The property is NO TRUNCATION AND NO DUPLICATION UNDER BACKPRESSURE. The shipped handler emitted
# every chunk in one call and drained the ring between them, which only worked because it could
# block the loop task it ran on. The reassembly check is what makes a dropped or repeated chunk
# visible -- a count alone would not.
add_executable(od_config_read_test config_read_test.c od_log_test_stub.c)
target_link_libraries(od_config_read_test od_session_fake)
add_test(NAME config_read COMMAND od_config_read_test)

# dispatch: the ordering in od_dispatch.h, which IS the specification.
#
# Every bug these guard against is an ordering bug with a specific consequence: reserve after the
# gate loses the gate's own FE/FF answer; reserve after the handler lets a handler mutate state it
# cannot then report; defer after decrypt advances the replay window so the re-dispatched frame is
# refused the second time; and a late producer-conflict check splices two configs into one
# CRC-valid read-back.
# The ordinary fixture remains INFO for route/corpus consumers. The ordering executable puts the
# DEBUG od_dispatch object first and captures records itself; the archive copy then stays unpulled.
add_library(od_session_fake_dispatch STATIC session_fake.c aes128.c)
target_link_libraries(od_session_fake_dispatch PUBLIC od_shared_dispatch_fixture)
add_library(od_session_fake_dispatch_silabs STATIC session_fake.c aes128.c)
target_link_libraries(od_session_fake_dispatch_silabs PUBLIC od_shared_dispatch_fixture_silabs)

add_executable(od_dispatch_test dispatch_test.c)
target_compile_definitions(od_dispatch_test PRIVATE OD_LOG_LEVEL=OD_LOG_DEBUG)
target_link_libraries(od_dispatch_test od_dispatch_log_debug od_session_fake_dispatch)
add_test(NAME dispatch COMMAND od_dispatch_test)

# The same ordering suite against BG22's actual shared ABI: 2 KB config buffer, three TX slots,
# and PIPE/PARTIAL/RXQ declined. This catches a capability define applied only to a test TU instead
# of to the shared objects whose routing and reservation table it changes.
add_library(od_session_fake_silabs STATIC session_fake.c aes128.c)
target_link_libraries(od_session_fake_silabs PUBLIC od_shared_silabs)
add_executable(od_dispatch_silabs_test dispatch_test.c)
target_link_libraries(od_dispatch_silabs_test od_session_fake_dispatch_silabs)
add_test(NAME dispatch_silabs COMMAND od_dispatch_silabs_test)

# dispatch_route: the opcode map, one case per canonical opcode.
#
# Separate from od_dispatch_test because it asks a different question. That suite drives ONE opcode
# through the ordering; this one drives EVERY opcode through the map and records which hook ran.
# The map used to be a switch inside each target, where an opcode could be routed on one target and
# unknown on the other with nothing to say so -- and "unknown" is wire-visible, since it carries no
# activity stamp. A new opcode must fail here until its route and capability behaviour are chosen.
add_executable(od_dispatch_route_test dispatch_route_test.c od_log_test_stub.c)
target_link_libraries(od_dispatch_route_test od_session_fake_dispatch)
add_test(NAME dispatch_route COMMAND od_dispatch_route_test)

# core_reset: the shared half of a teardown -- transfer, producer, egress and session, in that
# order.
#
# The cases that carry weight are the negative ones: RX must NOT be in it (its producer differs per
# target, so a shared reset would race one and reorder the other), and the session must go through
# od_session_clear() rather than a memset, or its prepared HAL key slot is stranded in a finite
# pool with nothing left naming it.
add_executable(od_core_reset_test core_reset_test.c od_xfer_app_test_stub.c od_log_test_stub.c
    od_nfc_app_test_stub.c)
target_link_libraries(od_core_reset_test od_session_fake)
add_test(NAME core_reset COMMAND od_core_reset_test)

# session: the handshake, KDF, replay window and CCM envelope against a fake crypto HAL built on
# the host AES core. DIFFERENTIAL where it matters -- the replay window is upstream Firmware's
# own nonce_window.h, ported verbatim, so this suite exercises the shipped state machine rather
# than a re-derivation of it.
#
# What a green run does NOT prove: that mbedTLS or PSA is driven correctly, that a PSA key policy
# is right, or that a real backend's tag matches this one. Those need the targets and C5/C6
# hardware. With the C0 capture skipped, the reference here is transcription plus a ported header,
# not bytes a device emitted.
# session_fake is its own library because tests/fuzz/ links the SAME fake. A second, subtly
# different fake would make a fuzz finding unreproducible in this suite -- which is where a
# finding has to land to stay fixed.
add_library(od_session_fake STATIC session_fake.c aes128.c)
target_link_libraries(od_session_fake PUBLIC od_shared)
target_include_directories(od_session_fake PUBLIC ${CMAKE_CURRENT_LIST_DIR})

add_executable(od_session_test session_test.c)
target_link_libraries(od_session_test od_session_fake)
add_test(NAME session COMMAND od_session_test)

# aes128: FIPS-197 known-answer vectors for the host AES core (tests/host/aes128.c).
#
# TEST SUPPORT, not a shared/ module -- but it is registered, and deliberately, because it is the
# foundation every session expectation is computed on: the KDF, the session id, the auth proof and
# every CCM tag come out of od_test_aes128_encrypt(). If it were wrong the session suite would
# still agree with itself and prove nothing, so its vectors are the standard's own rather than
# values this implementation produced.
#
# Never linked into firmware: the targets use mbedTLS and PSA. It is not constant time.
add_executable(od_aes128_test aes128_test.c aes128.c)
add_test(NAME aes128 COMMAND od_aes128_test)

# tlsr_aes_modes: the CMAC/CCM modes targets/telink-tlsr builds over the TLSR825x AES-ECB engine,
# checked against RFC 4493/3610 vectors and differentially against session_ccm_reference.inc.
# The host AES core stands in for the engine, so this proves the modes, not the engine driver.
add_executable(od_tlsr_aes_modes_test tlsr_aes_modes_test.c aes128.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../targets/telink-tlsr/src/od_aes_modes.c)
target_include_directories(od_tlsr_aes_modes_test PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../targets/telink-tlsr/src)
add_test(NAME tlsr_aes_modes COMMAND od_tlsr_aes_modes_test)

# tlsr_display: targets/telink-tlsr's image path end to end -- od_xfer hooks, the panel drivers
# imported from Firmware_NRF52, and the bit-banged SPI -- against a fake port that decodes the wire
# back into command/data bytes. The imported drivers keep their unused `epd` parameters, so
# -Wunused-parameter is relaxed for those files only.
set(TLSR_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../targets/telink-tlsr/src)
add_executable(od_tlsr_display_test tlsr_display_test.c
               ${TLSR_SRC}/od_xfer_tlsr.c
               ${TLSR_SRC}/epd/UC81xx.c ${TLSR_SRC}/epd/SSD16xx.c ${TLSR_SRC}/epd/TI97xx.c
               ${TLSR_SRC}/epd/epd_models.c ${TLSR_SRC}/epd/epd_io.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/core/od_color.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/core/od_boot_screen.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/core/od_boot_payload.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/qrcode/qrcode.c)
set_source_files_properties(${TLSR_SRC}/epd/UC81xx.c ${TLSR_SRC}/epd/SSD16xx.c
                            PROPERTIES COMPILE_OPTIONS -Wno-unused-parameter)
target_include_directories(od_tlsr_display_test PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/protocol
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/core
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/hal
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
                           ${TLSR_SRC} ${TLSR_SRC}/epd)
add_test(NAME tlsr_display COMMAND od_tlsr_display_test)

# tlsr_led: targets/telink-tlsr's LED adapter over the real shared runner, against a fake port --
# pins are outputs only while in use, PA7 goes back to SWS after every run, suspend is held only
# while a pattern runs.
add_executable(od_tlsr_led_test tlsr_led_test.c ${TLSR_SRC}/od_led_tlsr.c
               ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/core/od_led.c)
target_include_directories(od_tlsr_led_test PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/protocol
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/core
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../shared/hal
                           ${TLSR_SRC})
add_test(NAME tlsr_led COMMAND od_tlsr_led_test)

# tlsr_fmt: the target's own snprintf (the Telink SDK has none) against the host libc, renamed at
# compile time so both fit in one binary. Floats are formatted with integer arithmetic only.
add_executable(od_tlsr_fmt_test tlsr_fmt_test.c ${TLSR_SRC}/od_tlsr_fmt.c)
set_source_files_properties(${TLSR_SRC}/od_tlsr_fmt.c PROPERTIES
                            COMPILE_DEFINITIONS "snprintf=od_snprintf;vsnprintf=od_vsnprintf")
add_test(NAME tlsr_fmt COMMAND od_tlsr_fmt_test)

# advert: the 16-byte MSD encoder. DIFFERENTIAL -- the test carries an independent
# transcription of the shipped Zephyr/Silabs byte assembly and sweeps both encoders over the
# input space, because this is a promotion of bytes three targets already broadcast. A vector
# table derived from the new code would prove only self-consistency.
#
# What a green run does NOT prove: that any target calls it yet. All three still build their
# own copy; this pins the bytes so those swaps are mechanical.
add_executable(od_advert_test advert_test.c)
target_link_libraries(od_advert_test od_shared)
add_test(NAME advert COMMAND od_advert_test)

# watchdog: strike counting, safe mode, the retained-byte layout and breadcrumb caching, driven
# against a fake retained store and a fake WDT. SCAFFOLD -- no target implements od_hal_wdt.h
# yet, so a green run here says the policy is right, NOT that any device is protected.
#
# The boot-loop cases carry the weight. A watchdog that cannot escape a boot loop is worse than
# no watchdog: the device never advertises, is unreachable over BLE/DFU, and flattens its
# battery faster than if it had simply hung.
add_executable(od_watchdog_test watchdog_test.c)
target_link_libraries(od_watchdog_test od_shared)
add_test(NAME watchdog COMMAND od_watchdog_test)

# --------------------------------------------------------------------------------------------
# TARGET HAL suites. These compile PRODUCTION target sources against fake vendor APIs, which is
# the point: the defects they cover are lifecycle defects -- which key id is destroyed, whether a
# failure propagates -- and a test that re-implements the logic proves only that the copy agrees
# with itself. fake_psa/ is on the include path of these binaries ONLY; it is not visible to
# shared/ or to any target build.
add_library(od_fake_psa STATIC fake_psa/fake_psa.c)
target_include_directories(od_fake_psa PUBLIC ${CMAKE_CURRENT_LIST_DIR}/fake_psa)

# nordic_crypto_slot: the prepared-key slot lifecycle in the Nordic PSA HAL. The case that matters
# is the injected destroy failure -- not reproducible on a board, and the one that used to latch
# the slot ready-but-unusable until reboot, leaving the device unable to re-authenticate.
add_executable(od_nordic_crypto_slot_test
    nordic_crypto_slot_test.c
    ../../targets/nordic-zephyr/src/od_hal_crypto.c)
target_include_directories(od_nordic_crypto_slot_test PRIVATE
    ../../targets/nordic-zephyr/src)
target_link_libraries(od_nordic_crypto_slot_test od_shared od_fake_psa)
add_test(NAME nordic_crypto_slot COMMAND od_nordic_crypto_slot_test)

# esp_crypto_random: the ESP32 RNG arm, which feeds the AUTHENTICATE challenge. od_session's rule
# is that a device never offers a challenge it cannot honour, and that rule is unenforceable while
# the backend cannot report failure.
add_executable(od_esp_crypto_random_test
    esp_crypto_random_test.c
    od_log_test_stub.c
    ../../targets/esp32-idf/hal/od_hal_crypto_random.c)
target_include_directories(od_esp_crypto_random_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_esp)
target_link_libraries(od_esp_crypto_random_test od_shared od_fake_psa)
add_test(NAME esp_crypto_random COMMAND od_esp_crypto_random_test)

# nordic_session_app: the device-id packing, which is WIRE-VISIBLE -- four bytes of the silicon id
# feed both the KDF and the AUTHENTICATE proof, so a different selection is a different device to
# the host and every provisioned tag in a fleet becomes one no stored key opens. C11 moved that
# arithmetic between files, which is exactly the change that can alter it without altering anything
# a build or a boot would notice. Expectations are hand-computed, not taken from the code.
add_executable(od_nordic_session_app_test
    nordic_session_app_test.c
    ../../targets/nordic-zephyr/src/od_session_app.c)
target_include_directories(od_nordic_session_app_test PRIVATE
    ../../targets/nordic-zephyr/src
    ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr)
# od_session_fake, not od_shared: this pulls in od_session.c, which binds the crypto HAL at
# link time. The fake is the one every other session binary already uses.
target_link_libraries(od_nordic_session_app_test od_session_fake)
add_test(NAME nordic_session_app COMMAND od_nordic_session_app_test)

# nordic_config_parser: the loop thread publishes one derived security byte only after a config
# load completes; the BT RX thread's debug predicate reads that byte instead of the mutable
# aggregate. Production parser and predicate are both compiled here.
add_executable(od_nordic_config_parser_test
    nordic_config_parser_test.c
    ../../targets/nordic-zephyr/src/opendisplay_config_parser.c
    ../../targets/nordic-zephyr/src/od_rxq_app.c)
target_include_directories(od_nordic_config_parser_test PRIVATE
    ../../targets/nordic-zephyr/src)
target_link_libraries(od_nordic_config_parser_test od_session_fake)
add_test(NAME nordic_config_parser COMMAND od_nordic_config_parser_test)

# nordic_cmd_device: what this target's device commands ANSWER, which dispatch_route_test cannot
# see -- that suite replaces every hook with a recorder, so it proves routing and nothing about
# behaviour. Two answers here were decided deliberately and had no other coverage: 0x52's
# unsupported NACK (a C11 wire change) and 0x53's deliberate silence. 0x43 is included because
# ESP32 answered nothing to it from C8 to C11.2 and no suite noticed.
add_executable(od_nordic_cmd_device_test
    nordic_cmd_device_test.c
    ../../targets/nordic-zephyr/src/od_cmd_device.c)
target_include_directories(od_nordic_cmd_device_test PRIVATE
    ../../targets/nordic-zephyr/src
    ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr)
target_link_libraries(od_nordic_cmd_device_test od_shared)
add_test(NAME nordic_cmd_device COMMAND od_nordic_cmd_device_test)

# --------------------------------------------------------------------------------------------
# dispatch_corpus: tests/vectors/dispatch.json replayed through the production od_dispatch_frame().
#
# This is the OTHER END of a contract that has only ever been checked from one side.
# replay_vectors.py drives the same corpus through py-opendisplay's public API and says outright
# that firmware replies are "never checkable here" (replay_vectors.py:18) -- so every h2d
# expect.reply has been unchecked against firmware since the corpus was authored. Same file, same
# vectors, the device side.
#
# The table is GENERATED into the build directory and never committed: a checked-in copy is the
# second corpus TEST_OWNERSHIP.md forbids, and it permits a stale green. Regenerating depends on the
# JSON, so editing a vector rebuilds the table.
# REQUIRED, not optional. A warning here used to leave the schema gate, both corpus executables and
# the bench test out of a build that still configured and reported green -- which is the whole
# C12 firmware-reply and bench coverage vanishing without a failure. That is the "a skip is not a
# pass" rule of tools/check.sh, applied where it was quietly not.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(Python3_FOUND)
    set(OD_VEC_TOOL ${CMAKE_CURRENT_LIST_DIR}/vectors_tool.py)
    set(OD_VEC_DIR  ${CMAKE_CURRENT_LIST_DIR}/../vectors)
    set(OD_VEC_INC  ${CMAKE_CURRENT_BINARY_DIR}/generated/dispatch_vectors.inc)
    # CONFIGURE_DEPENDS: without it a vector file added after configuration is seen by the runtime
    # schema check but absent from the already-generated table, so the two halves disagree about
    # what the corpus is.
    file(GLOB OD_VEC_JSON CONFIGURE_DEPENDS ${OD_VEC_DIR}/*.json)
    add_custom_command(
        OUTPUT ${OD_VEC_INC}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
        COMMAND ${Python3_EXECUTABLE} ${OD_VEC_TOOL} --emit ${OD_VEC_DIR} ${OD_VEC_INC}
        DEPENDS ${OD_VEC_TOOL} ${OD_VEC_JSON}
        COMMENT "od: generating the dispatch vector table from tests/vectors/")
    add_custom_target(od_vectors_generated DEPENDS ${OD_VEC_INC})

    # The schema gate, as its own test: a malformed or under-specified vector must fail loudly
    # rather than be silently dropped by whatever consumes it next.
    # --audit, not --check: the corpus-wide invariants (exact counts, and the paired vectors that
    # must not drift apart) belong to THE corpus, and only the real one.
    add_test(NAME vectors_schema
             COMMAND ${Python3_EXECUTABLE} ${OD_VEC_TOOL} --audit ${OD_VEC_DIR})

    # ONE runner source, THREE executables. od_cmd_app_* is static link-time composition, so
    # profiles cannot share a binary -- and the answer to that is emphatically not a runtime
    # registry. Each executable resolves the seam once.
    #
    # Only the runner sees the generated table. A profile gets corpus_runner.h, which exposes
    # semantic knobs and no expected bytes: a fake that could read expect.reply would make the
    # corpus its own oracle, and every vector would pass while proving nothing.
    # THE GENERATED DIRECTORY GOES ON ONE SOURCE, not the target. target_include_directories()
    # applies to every source in the target, so the profiles and fakes could reach
    # dispatch_vectors.inc and consult the answers -- the anti-oracle property was documented and
    # not enforced. A source-level property scopes it to the runner, which is the only translation
    # unit entitled to see an expectation.
    set_source_files_properties(corpus_runner.c PROPERTIES
        INCLUDE_DIRECTORIES ${CMAKE_CURRENT_BINARY_DIR}/generated)

    # od_nfc.c is compiled in HERE at OD_CAP_NFC=0 rather than stubbed. The capability-off vector
    # claims shared/core/od_nfc.c emits nothing, and a handwritten stub agreeing with that claim
    # would leave a regression in the real off arm green -- the fake would be its own oracle. The
    # off arm needs no seam fake because it references no seam.
    add_executable(od_dispatch_corpus_portable_test
        corpus_runner.c
        corpus_profile_portable.c
        od_log_test_stub.c
        ../../shared/core/od_nfc.c
        ${OD_VEC_INC})
    # PER-TARGET, not per-source: set_source_files_properties is DIRECTORY-scoped, so setting the
    # capability on this path would compile od_nfc.c capability-off for every target in this file
    # that also builds it -- including od_nfc_test and the shared archive.
    target_compile_definitions(od_dispatch_corpus_portable_test PRIVATE OD_CAP_NFC=0)
    target_link_libraries(od_dispatch_corpus_portable_test od_session_fake_dispatch)
    add_dependencies(od_dispatch_corpus_portable_test od_vectors_generated)
    add_test(NAME dispatch_corpus_portable COMMAND od_dispatch_corpus_portable_test)

    # The second executable, and the one whose green means "the firmware emits these bytes". It
    # defines no hook of its own: every od_cmd_app_* comes from Nordic's production command
    # translation units, compiled against fake drivers. That follows the rule the target lifecycle
    # suites already set -- compile the production source, never a transcription.
    add_executable(od_dispatch_corpus_nordic_test
        corpus_runner.c
        corpus_profile_nordic.c
        fake_nordic/fake_nordic.c
        ../../targets/nordic-zephyr/src/od_cmd_device.c
        ../../targets/nordic-zephyr/src/od_cmd_config.c
        ../../targets/nordic-zephyr/src/od_cmd_reply.c
        ../../targets/nordic-zephyr/src/od_inflate_app.c
        ${OD_VEC_INC})
    target_include_directories(od_dispatch_corpus_nordic_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fake_nordic
        ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr
        ../../targets/nordic-zephyr/src)
    # The build SHA is a COMPILE-TIME macro on this target, not a runtime knob like the version and
    # patch fields -- so the vector that pins the current 0x43 answer needs it defined here, on
    # this executable only. Setting it globally would reach the other Nordic suites, which
    # deliberately run against the default placeholder.
    target_compile_definitions(od_dispatch_corpus_nordic_test PRIVATE SHA="1a2b3c4d")
    target_compile_options(od_dispatch_corpus_nordic_test PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
    target_link_libraries(od_dispatch_corpus_nordic_test od_session_fake)
    add_dependencies(od_dispatch_corpus_nordic_test od_vectors_generated)
    add_test(NAME dispatch_corpus_nordic COMMAND od_dispatch_corpus_nordic_test)

    # The tag fake behind od_nfc_app.h, and its own contract. Compiled now, before step 3's shared
    # machine exists, so a broken seam or knob surfaces here rather than inside the first suite
    # built on it.
    add_library(od_fake_nfc_tag STATIC fake_nfc_tag.c)
    target_include_directories(od_fake_nfc_tag PUBLIC ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(od_fake_nfc_tag PUBLIC od_shared)

    add_executable(od_nfc_fake_test nfc_fake_test.c)
    target_link_libraries(od_nfc_fake_test od_fake_nfc_tag)
    add_test(NAME nfc_fake COMMAND od_nfc_fake_test)

    # nfc: the shared 0x0083 machine at OD_CAP_NFC=1, over the programmable tag.
    # This suite captures the shared lifecycle records exactly as well as exercising the wire
    # machine, so it supplies _od_log itself.
    add_executable(od_nfc_test nfc_test.c)
    target_link_libraries(od_nfc_test od_fake_nfc_tag od_session_fake)
    add_test(NAME nfc COMMAND od_nfc_test)

    # nfc_off: the same machine at OD_CAP_NFC=0. od_nfc.c is compiled INTO the executable rather
    # than taken from od_shared, because neither shared archive carries this profile -- od_shared is
    # capability-on and od_shared_silabs is BG22, which has NFC. Defining OD_CAP_NFC=0 on an
    # executable that linked a capability-on archive would prove nothing and still pass.
    add_executable(od_nfc_off_test nfc_off_test.c ../../shared/core/od_nfc.c)
    target_compile_definitions(od_nfc_off_test PRIVATE OD_CAP_NFC=0)
    target_link_libraries(od_nfc_off_test od_session_fake)
    add_test(NAME nfc_off COMMAND od_nfc_off_test)

    # nordic_nfc: the 0x0083 length bounds, against Nordic's production handler over fake drivers.
    # Focused rather than a corpus vector because what it asserts is that a refused frame reached
    # NO TAG SINK -- an absence the wire bytes cannot show.
    add_executable(od_nordic_nfc_test
        nordic_nfc_test.c
        fake_nordic/fake_nordic.c
        ../../targets/nordic-zephyr/src/od_cmd_reply.c)
    target_include_directories(od_nordic_nfc_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fake_nordic
        ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr
        ../../targets/nordic-zephyr/src)
    target_link_libraries(od_nordic_nfc_test od_session_fake)
    add_test(NAME nordic_nfc COMMAND od_nordic_nfc_test)

    # The BG22 production hooks, compiled with the same 2 KiB/three-slot/no-PIPE ABI as the target.
    # Only the vendor drivers beneath them are faked; reply bytes still come from od_cmd_silabs.c.
    add_executable(od_dispatch_corpus_silabs_test
        corpus_runner.c
        corpus_profile_silabs.c
        fake_silabs/fake_silabs.c
        ../../targets/efr32bg22-slc/od_cmd_silabs.c
        ../../targets/efr32bg22-slc/od_inflate_app.c
        ${OD_VEC_INC})
    target_include_directories(od_dispatch_corpus_silabs_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fake_silabs
        ../../targets/efr32bg22-slc)
    target_compile_definitions(od_dispatch_corpus_silabs_test PRIVATE
        OPENDISPLAY_BUILD_ID="1a2b3c4d")
    target_link_libraries(od_dispatch_corpus_silabs_test od_session_fake_silabs)
    add_dependencies(od_dispatch_corpus_silabs_test od_vectors_generated)
    add_test(NAME dispatch_corpus_silabs COMMAND od_dispatch_corpus_silabs_test)

    # Target-specific failure policy: production BGAPI transport + production command hooks over
    # scripted vendor drivers. This is separate from the corpus because ordering, time and driver
    # failures are state assertions rather than wire fixtures.
    add_executable(od_silabs_fault_test
        silabs_fault_test.c
        fake_silabs/fake_silabs.c
        fake_silabs/fake_silabs_bgapi.c
        ../../targets/efr32bg22-slc/od_cmd_silabs.c
        ../../targets/efr32bg22-slc/od_inflate_app.c
        ../../targets/efr32bg22-slc/opendisplay_pipe.c
        ../../targets/efr32bg22-slc/od_hal_radio.c)
    target_include_directories(od_silabs_fault_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fake_silabs
        ../../targets/efr32bg22-slc)
    target_compile_definitions(od_silabs_fault_test PRIVATE
        OD_TEST_REAL_SILABS_PIPE=1
        OPENDISPLAY_BUILD_ID="1a2b3c4d")
    target_link_libraries(od_silabs_fault_test od_session_fake_silabs)
    add_test(NAME silabs_fault COMMAND od_silabs_fault_test)
endif()

# The shared LED runner. Vendor drivers are gone from this suite entirely: the machine's only
# seam is od_led_app, so the fake is three functions and a millisecond clock the test advances
# itself. An unbounded loop does not fail on its own, hence the timeout.
add_executable(od_led_test led_test.c)
target_link_libraries(od_led_test od_shared)
add_test(NAME led COMMAND od_led_test)
set_tests_properties(led PROPERTIES TIMEOUT 30)

# Shared buzzer policy: the live Firmware/host exponential scale, octave folding, parse and the
# 30-second cap. Hardware PWM remains below od_buzzer_app and is gated per target.
add_executable(od_buzzer_test buzzer_test.c)
target_link_libraries(od_buzzer_test od_shared m)
add_test(NAME buzzer COMMAND od_buzzer_test)
set_tests_properties(buzzer PROPERTIES TIMEOUT 30)

# The BG22 adapter's scheduler, against the production source. led_test covers the machine; this
# covers turning its returned delay into a sleeptimer arm, and re-arming on every expiry -- an
# adapter that stops re-arming leaves a superloop with no watchdog silently stuck.
add_executable(od_silabs_led_adapter_test
    silabs_led_adapter_test.c
    ../../targets/efr32bg22-slc/opendisplay_led.c)
target_include_directories(od_silabs_led_adapter_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/fake_silabs
    ../../targets/efr32bg22-slc)
target_link_libraries(od_silabs_led_adapter_test od_shared_silabs)
add_test(NAME silabs_led_adapter COMMAND od_silabs_led_adapter_test)
set_tests_properties(silabs_led_adapter PROPERTIES TIMEOUT 30)

# bench_dispatch_gate: the two `dispatch-gate` helpers against a fake BLE client.
#
# The tool exists to produce HARDWARE EVIDENCE, and it can drive its stimulus wrongly in two ways
# that look identical in a device log: sealing twice instead of resending retained bytes (which
# tests PIPE duplicate handling, not the replay window), and writing after re-enabling notifications
# instead of before (which withholds nothing). Neither is visible on a board. Both are visible here,
# which is the point of running it before either goes near one.
#
# bleak is stubbed rather than required, so this never degrades to a skip on a machine without the
# BLE stack.
add_test(NAME bench_dispatch_gate
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_LIST_DIR}/bench_dispatch_gate_test.py)

# nfc_read_tool: the nfc-read reply decoder. It is the ORACLE for every NFC read hardware row --
# py-opendisplay implements no NFC_SUB_READ -- so a decoder that trusted the declared length would
# pass a device that framed 218 and sent 200, the exact defect those rows exist to catch.
add_test(NAME nfc_read_tool
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_LIST_DIR}/nfc_read_tool_test.py)

# vectors_schema: positive AND negative fixtures. A validator is worth its REJECTIONS, and a suite
# that only feeds it the real corpus proves it accepts what already exists -- which it must, or the
# tree would be red. The C12 review found five malformed shapes it accepted; each has a case.
add_test(NAME vectors_schema_fixtures
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_LIST_DIR}/vectors_schema_test.py)

# Nordic config pin bytes are a wire contract and deliberately differ by
# platform: nRF52840 uses absolute port*32+pin values, while nRF54 uses the
# compact packed form. Keep the pure decoder checked without a Zephyr build.
add_executable(od_nordic_pin_codec_test
    nordic_pin_codec_test.c
    ../../targets/nordic-zephyr/src/od_pin_codec.c)

    # nordic_nfc_pins: the NFC antenna reservation in the nRF52840 platform decoder. That file was
    # not compiled by any host test, so the one `if` deciding whether a config may claim P0.09/P0.10
    # had no coverage -- and the failure it prevents is a GPIO write that succeeds and drives
    # nothing. Built at OD_CAP-equivalent NFC-on, which is how every shipping image is built.
    add_executable(od_nordic_nfc_pins_test
        nordic_nfc_pins_test.c
        ../../targets/nordic-zephyr/src/od_pin_codec.c
        ../../targets/nordic-zephyr/src/platform/nrf52840/od_pin_codec_nrf52840.c)
    target_include_directories(od_nordic_nfc_pins_test PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/fake_zephyr
        ../../targets/nordic-zephyr/src)
    target_compile_definitions(od_nordic_nfc_pins_test PRIVATE
        CONFIG_OD_PLATFORM_NRF52840=1
        CONFIG_OD_PLATFORM_NRF54=0
        CONFIG_NFC_T2T_NRFXLIB=1)
    add_test(NAME nordic_nfc_pins COMMAND od_nordic_nfc_pins_test)
target_include_directories(od_nordic_pin_codec_test PRIVATE
    ../../targets/nordic-zephyr/src)
add_test(NAME nordic_pin_codec COMMAND od_nordic_pin_codec_test)

# replay_vectors: the corpus replayed through py-opendisplay's public API. This is the
# dual-stack half of the contract test — the same vectors will later run against the C
# core, and a vector the two sides disagree on is exactly the host/firmware divergence
# this repo exists to eliminate.
#
# Optional by design: it needs Python plus a pinned py-opendisplay from PyPI, which is a
# heavier dependency than a C build should mandate. CI always runs it; a developer with
# neither still gets the compile gate. Absence is reported, never silently skipped —
# a test that vanishes quietly is how coverage rots.
find_package(Python3 COMPONENTS Interpreter)

set(OD_REPLAY_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/replay_vectors.py)
set(OD_VECTORS_DIR   ${CMAKE_CURRENT_LIST_DIR}/../vectors)

if(Python3_FOUND AND EXISTS ${OD_REPLAY_SCRIPT})
    # py-opendisplay must be importable by *this* interpreter. It declares
    # requires-python >=3.13, so a distro python of 3.12 or older cannot host it at all —
    # which is the common local case and must not present as a test failure.
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import opendisplay"
        RESULT_VARIABLE OD_PYOD_IMPORT
        OUTPUT_QUIET ERROR_QUIET)

    if(OD_PYOD_IMPORT EQUAL 0)
        add_test(NAME replay_vectors
                 COMMAND ${Python3_EXECUTABLE} ${OD_REPLAY_SCRIPT} ${OD_VECTORS_DIR})
        # The runner exits non-zero on failure and prints SKIP lines for vectors it cannot
        # express against the host API; those are not failures. It also exits non-zero on an
        # empty corpus — a runner that is green because it found nothing is the exact
        # failure mode TEST_OWNERSHIP.md indicts in py-opendisplay's own fixture fallbacks.
        message(STATUS "od: replay_vectors test registered (${Python3_EXECUTABLE})")
    else()
        message(WARNING
            "od: py-opendisplay is not importable by ${Python3_EXECUTABLE} "
            "(v${Python3_VERSION}; py-opendisplay 7.14.0 needs >=3.13) — replay_vectors "
            "NOT registered. The corpus is UNCHECKED in this build. CI always runs it; "
            "to run it here: uv run --python 3.13 --with py-opendisplay==7.14.0 "
            "python tests/host/replay_vectors.py tests/vectors")
    endif()
elseif(NOT Python3_FOUND)
    message(WARNING "od: Python3 not found — replay_vectors NOT registered. "
                    "The corpus is unchecked in this build.")
else()
    message(WARNING "od: ${OD_REPLAY_SCRIPT} missing — replay_vectors NOT registered.")
endif()

# --------------------------------------------------------------------------------------------
# Fuzz targets (tests/fuzz/), Clang only -- -fsanitize=fuzzer is a Clang feature and gcc has no
# libFuzzer. Added here rather than as a separate project so the SAME configure step CI already
# runs builds them, and so they link od_session_fake instead of a second, divergent fake.
if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/../fuzz/CMakeLists.txt)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../fuzz fuzz)
    message(STATUS "od: Clang detected — tests/fuzz targets registered")
else()
    message(STATUS "od: fuzz targets NOT built (needs Clang); the pre-auth surface is "
                   "UNFUZZED in this build")
endif()
