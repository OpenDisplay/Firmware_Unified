# Host coverage for the config-driven 3.0 stack: od_cfg_index, od_epd_cfg /
# od_epd (golden panel traces), od_cmd_drive / od_cmd_spi_raw, od_imgstore / od_flash_nor and
# od_action. Included from CMakeLists.txt; everything here links the same od_shared archives
# as the rest of the gate. drive_fixture.h (the fake medium, config builder and recording bus)
# is single-TU, so each suite is one test source plus the production files it covers.
#
# Suites named *_test carry KNOWN_BUG markers for defects found here (strict: an XPASS fails the
# suite so a fixed bug cannot leave its marker behind). The bug list is in each marker's id.

set(OD_SHARED_CORE ${CMAKE_CURRENT_LIST_DIR}/../../shared/core)

# Both config caps: 4096 (ESP32, Nordic, host default) and BG22's 3120.
foreach(variant IN ITEMS default silabs)
    if(variant STREQUAL "silabs")
        set(lib od_shared_silabs)
        set(sfx _bg22)
    else()
        set(lib od_shared)
        set(sfx "")
    endif()
    add_executable(od_cfg_index${sfx}_test cfg_index_test.c od_log_test_stub.c
        ${OD_SHARED_CORE}/od_cfg_index.c)
    target_link_libraries(od_cfg_index${sfx}_test ${lib})
    add_test(NAME cfg_index${sfx} COMMAND od_cfg_index${sfx}_test)
endforeach()

# od_epd_cfg + od_epd over the representative real panels (golden traces) and synthetic edges.
# Default profile only: the BG22 interpreter differs only by OD_EPD_WITH_WINDOW=0, a target
# compile definition outside od_shared_silabs; tests/panels replays every panel through that build.
add_executable(od_epd_cfg_test epd_cfg_test.c od_log_test_stub.c
    ${OD_SHARED_CORE}/od_epd_cfg.c ${OD_SHARED_CORE}/od_epd.c ${OD_SHARED_CORE}/od_cfg_index.c)
target_link_libraries(od_epd_cfg_test od_shared)
target_compile_definitions(od_epd_cfg_test PRIVATE
    OD_GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/../vectors/epd_golden")
add_test(NAME epd_cfg COMMAND od_epd_cfg_test)

# od_action over a stored 0xB2 table: every trigger, temperature edges, validator vs runner,
# uptime wrap, and the shared CMD_ACTION_INFO / CMD_ACTION_FIRE handlers. Twice: with an image
# store (BG22), and without one (ESP32), where DISPLAY_SLOT entries load as inactive. No other
# suite links od_action.c.
foreach(img IN ITEMS 1 0)
    if(img)
        set(sfx "")
    else()
        set(sfx _no_imgstore)
    endif()
    add_executable(od_action${sfx}_test action_test.c od_log_test_stub.c
        ${OD_SHARED_CORE}/od_action.c ${OD_SHARED_CORE}/od_cmd_action.c
        ${OD_SHARED_CORE}/od_cfg_index.c)
    target_link_libraries(od_action${sfx}_test od_shared)
    target_compile_definitions(od_action${sfx}_test PRIVATE OD_CAP_IMGSTORE=${img} OD_CAP_ACTION=1)
    add_test(NAME action${sfx} COMMAND od_action${sfx}_test)
endforeach()

# od_imgstore + od_flash_nor over a simulated SPI NOR (the od_hal_spi seam), both header
# layouts: default, and the write-once layout the Nordic XIAO (Puya P25Q16H) build uses.
foreach(wo IN ITEMS 0 1)
    if(wo)
        set(sfx _write_once)
    else()
        set(sfx "")
    endif()
    add_executable(od_imgstore_nor${sfx}_test imgstore_nor_test.c od_log_test_stub.c
        ${OD_SHARED_CORE}/od_imgstore.c ${OD_SHARED_CORE}/od_flash_nor.c)
    target_link_libraries(od_imgstore_nor${sfx}_test od_shared)
    target_compile_definitions(od_imgstore_nor${sfx}_test PRIVATE
        OD_IMGSTORE_WRITE_ONCE_HEADER=${wo})
    add_test(NAME imgstore_nor${sfx} COMMAND od_imgstore_nor${sfx}_test)
endforeach()

# The shared CMD_DRIVE_WRITE_START (0x0085) and CMD_SPI_RAW (0x0084) handlers over real
# od_epd_cfg / od_epd / od_xfer. od_shared_silabs is the host archive built with
# OD_CAP_DRIVE_WRITE and OD_CAP_SPI_RAW; neither handler has BG22-specific code.
add_executable(od_drive_cmd_test drive_cmd_test.c od_log_test_stub.c
    ${OD_SHARED_CORE}/od_cmd_drive.c ${OD_SHARED_CORE}/od_cmd_spi_raw.c
    ${OD_SHARED_CORE}/od_epd_cfg.c ${OD_SHARED_CORE}/od_epd.c ${OD_SHARED_CORE}/od_cfg_index.c)
target_link_libraries(od_drive_cmd_test od_shared_silabs)
add_test(NAME drive_cmd COMMAND od_drive_cmd_test)

# CMD_READ_IDENT on the default profile: the 3.0 tail, fw_target, NEEDS_MIGRATION from the mode-0
# rule, BUILTIN_DRIVER. od_shared is built without the cap, so the handler is compiled here with it.
add_executable(od_ident_test ident_test.c od_log_test_stub.c
    ${OD_SHARED_CORE}/od_cmd_ident.c ${OD_SHARED_CORE}/od_cfg_index.c)
target_link_libraries(od_ident_test od_shared)
target_compile_definitions(od_ident_test PRIVATE
    OD_CAP_READ_IDENT=1 OD_IDENT_NATIVE_PANELS=1 OD_IDENT_TARGET=OD_IDENT_TARGET_NRF52840)
add_test(NAME ident COMMAND od_ident_test)
