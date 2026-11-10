# libFuzzer targets for od_session -- the pre-authentication surface MIGRATION.md Gate 1 names.
#
# Added from tests/host/CMakeLists.txt and only under Clang, because -fsanitize=fuzzer is a Clang
# feature; gcc has no libFuzzer. The gcc job therefore skips these entirely, and the Clang job
# BUILDS them on every push even when it does not run them -- which is what keeps them from
# rotting into code nobody compiles.
#
# They link od_session_fake, the SAME fake the unit suite uses. A separate fake would make a
# finding here unreproducible in session_test.c, which is where a finding has to land to stay
# fixed.
#
# Run one directly:
#   build/fuzz/od_fuzz_session_open_sealed -max_total_time=60 tests/fuzz/corpus/session_open_sealed
# A crash writes its input to ./crash-<sha1>; add it to the corpus AND pin it as a numbered case
# in session_test.c, so the regression fails the ordinary gcc+clang run rather than only a fuzz
# run nobody triggers.

set(OD_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)

# od_log_test_stub.c is listed here, not carried by od_session_fake: an archive member is only
# extracted for a symbol already undefined when the linker reaches it, and nothing in a fuzz
# harness references _od_log, so a stub inside the fake would be skipped and od_log.c pulled from
# od_shared with no transport under it.
foreach(target session_open_raw session_open_sealed session_auth)
    add_executable(od_fuzz_${target} fuzz_${target}.c ../host/od_log_test_stub.c)
    target_link_libraries(od_fuzz_${target} od_session_fake)
    target_include_directories(od_fuzz_${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(od_fuzz_${target} PRIVATE ${OD_FUZZ_FLAGS})
    target_link_options(od_fuzz_${target} PRIVATE ${OD_FUZZ_FLAGS})

    # A very short run under ctest, so `ctest` alone proves each target still starts, opens its
    # session and survives its seeds. It is a smoke test, NOT the fuzzing -- the real budget is
    # the fuzz-short CI job and manual soaks. -runs is bounded (not -max_total_time) so the
    # ordinary test run stays deterministic in length.
    add_test(NAME fuzz_${target}
             COMMAND od_fuzz_${target} -runs=2000 -print_final_stats=0
                     ${CMAKE_CURRENT_LIST_DIR}/corpus/${target})
    set_tests_properties(fuzz_${target} PROPERTIES
                         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endforeach()

foreach(target pipe_start pipe_data)
    add_executable(od_fuzz_${target}
        fuzz_${target}.c
        fuzz_pipe_support.c)
    target_link_libraries(od_fuzz_${target} od_shared)
    target_include_directories(od_fuzz_${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_options(od_fuzz_${target} PRIVATE ${OD_FUZZ_FLAGS})
    target_link_options(od_fuzz_${target} PRIVATE ${OD_FUZZ_FLAGS})
    add_test(NAME fuzz_${target}
             COMMAND od_fuzz_${target} -runs=2000 -print_final_stats=0
                     ${CMAKE_CURRENT_LIST_DIR}/corpus/${target})
    set_tests_properties(fuzz_${target} PROPERTIES
                         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endforeach()

# The stored config through everything that reads it on demand: od_cfg_index (differential
# against od_config_tlv_walk), od_epd_cfg + od_epd, od_action and od_config_store_save_carry.
# drive_fixture.h supplies the fake medium and the recording panel bus.
set(OD_FUZZ_CORE ${CMAKE_CURRENT_LIST_DIR}/../../shared/core)
set(OD_FUZZ_DRIVE_SOURCES
    ${OD_FUZZ_CORE}/od_cfg_index.c
    ${OD_FUZZ_CORE}/od_epd.c
    ${OD_FUZZ_CORE}/od_epd_cfg.c
    ${OD_FUZZ_CORE}/od_action.c
    ${OD_FUZZ_CORE}/od_config_carry.c
    ../host/od_log_test_stub.c)
add_executable(od_fuzz_config_drive fuzz_config_drive.c ${OD_FUZZ_DRIVE_SOURCES})
target_link_libraries(od_fuzz_config_drive od_shared)
target_include_directories(od_fuzz_config_drive PRIVATE ${CMAKE_CURRENT_LIST_DIR}
                           ${CMAKE_CURRENT_LIST_DIR}/../host)
target_compile_options(od_fuzz_config_drive PRIVATE ${OD_FUZZ_FLAGS})
target_link_options(od_fuzz_config_drive PRIVATE ${OD_FUZZ_FLAGS})
add_test(NAME fuzz_config_drive
         COMMAND od_fuzz_config_drive -runs=2000 -print_final_stats=0
                 ${CMAKE_CURRENT_LIST_DIR}/corpus/config_drive)
set_tests_properties(fuzz_config_drive PROPERTIES
                     ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

add_executable(od_fuzz_seed_config_drive seed_config_drive.c
    ${OD_FUZZ_CORE}/od_cfg_index.c ../host/od_log_test_stub.c)
target_link_libraries(od_fuzz_seed_config_drive od_shared)
target_include_directories(od_fuzz_seed_config_drive PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../host)
