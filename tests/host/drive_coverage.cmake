# Host coverage for the config-driven 3.0 stack: od_cfg_index, od_config_carry, od_epd_cfg /
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

foreach(variant IN ITEMS default silabs)
    if(variant STREQUAL "silabs")
        set(lib od_shared_silabs)
        set(sfx _bg22)
    else()
        set(lib od_shared)
        set(sfx "")
    endif()
    add_executable(od_config_carry_adversarial${sfx}_test config_carry_adversarial_test.c
        od_log_test_stub.c ${OD_SHARED_CORE}/od_config_carry.c ${OD_SHARED_CORE}/od_cfg_index.c)
    target_link_libraries(od_config_carry_adversarial${sfx}_test ${lib})
    add_test(NAME config_carry_adversarial${sfx} COMMAND od_config_carry_adversarial${sfx}_test)
endforeach()

# od_epd_cfg + od_epd over the representative real panels (golden traces) and synthetic edges.
# Default profile only: BG22's smaller OD_EPD_SEQ_MAX is a target compile definition, not part
# of od_shared_silabs, so a _bg22 variant here would not test the BG22 interpreter limits.
add_executable(od_epd_cfg_test epd_cfg_test.c od_log_test_stub.c
    ${OD_SHARED_CORE}/od_epd_cfg.c ${OD_SHARED_CORE}/od_epd.c ${OD_SHARED_CORE}/od_cfg_index.c)
target_link_libraries(od_epd_cfg_test od_shared)
target_compile_definitions(od_epd_cfg_test PRIVATE
    OD_GOLDEN_DIR="${CMAKE_CURRENT_LIST_DIR}/../vectors/epd_golden")
add_test(NAME epd_cfg COMMAND od_epd_cfg_test)
