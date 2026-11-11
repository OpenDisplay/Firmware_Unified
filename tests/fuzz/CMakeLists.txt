# libFuzzer targets for the pre-authentication surface MIGRATION.md Gate 1 names: SEC2 frames,
# PIPE and the stored config.
#
# Added from tests/host/CMakeLists.txt and only under Clang, because -fsanitize=fuzzer is a Clang
# feature; gcc has no libFuzzer. The gcc job therefore skips these entirely, and the Clang job
# BUILDS them on every push even when it does not run them -- which is what keeps them from
# rotting into code nobody compiles.
#
# The SEC2 target links od_session_fake_dispatch and sec2_test.c's harness, the SAME ones the
# unit suite uses, so a finding reproduces in sec2_test.c, where it has to land to stay fixed.
#
# Run one directly:
#   build/fuzz/od_fuzz_sec2_frames -max_total_time=60 tests/fuzz/corpus/sec2_frames
# A crash writes its input to ./crash-<sha1>; add it to the corpus AND pin it as a numbered case
# in sec2_test.c, so the regression fails the ordinary gcc+clang run rather than only a fuzz
# run nobody triggers.

set(OD_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -g)

# The SEC2 sources are compiled INTO the target, not taken from the archive, so libFuzzer's
# coverage instrumentation reaches the code under test and not only the harness.
set(OD_FUZZ_SEC2 ${CMAKE_CURRENT_LIST_DIR}/../../shared/core)
add_executable(od_fuzz_sec2_frames fuzz_sec2_frames.c ../host/od_log_test_stub.c
    ${OD_FUZZ_SEC2}/od_sec2.c
    ${OD_FUZZ_SEC2}/od_sec2_rec.c
    ${OD_FUZZ_SEC2}/od_sec2_p256.c
    ${OD_FUZZ_SEC2}/od_gate.c
    ${OD_FUZZ_SEC2}/od_reply.c
    ${OD_FUZZ_SEC2}/od_dispatch.c)
target_link_libraries(od_fuzz_sec2_frames od_session_fake_dispatch)
target_include_directories(od_fuzz_sec2_frames PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(od_fuzz_sec2_frames PRIVATE ${OD_FUZZ_FLAGS})
target_link_options(od_fuzz_sec2_frames PRIVATE ${OD_FUZZ_FLAGS})
# A very short run under ctest proves the target starts, authenticates and survives its seeds.
# It is a smoke test, NOT the fuzzing -- that is tools/check.sh's fuzz step and manual soaks.
add_test(NAME fuzz_sec2_frames
         COMMAND od_fuzz_sec2_frames -runs=2000 -print_final_stats=0
                 ${CMAKE_CURRENT_LIST_DIR}/corpus/sec2_frames)
set_tests_properties(fuzz_sec2_frames PROPERTIES
                     ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

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
# against od_config_tlv_walk), od_epd_cfg + od_epd, and od_action.
# drive_fixture.h supplies the fake medium and the recording panel bus.
set(OD_FUZZ_CORE ${CMAKE_CURRENT_LIST_DIR}/../../shared/core)
set(OD_FUZZ_DRIVE_SOURCES
    ${OD_FUZZ_CORE}/od_cfg_index.c
    ${OD_FUZZ_CORE}/od_epd.c
    ${OD_FUZZ_CORE}/od_epd_cfg.c
    ${OD_FUZZ_CORE}/od_action.c
    ../host/od_log_test_stub.c)
add_executable(od_fuzz_config_drive fuzz_config_drive.c ${OD_FUZZ_DRIVE_SOURCES})
target_link_libraries(od_fuzz_config_drive od_shared)
target_include_directories(od_fuzz_config_drive PRIVATE ${CMAKE_CURRENT_LIST_DIR}
                           ${CMAKE_CURRENT_LIST_DIR}/../host)
target_compile_options(od_fuzz_config_drive PRIVATE ${OD_FUZZ_FLAGS})
target_link_options(od_fuzz_config_drive PRIVATE ${OD_FUZZ_FLAGS})
# New units go to a build-tree directory (libFuzzer writes into the FIRST corpus dir), so a
# gate run never dirties the committed corpus.
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/config_drive_new)
add_test(NAME fuzz_config_drive
         COMMAND od_fuzz_config_drive -runs=2000 -print_final_stats=0
                 ${CMAKE_CURRENT_BINARY_DIR}/config_drive_new
                 ${CMAKE_CURRENT_LIST_DIR}/corpus/config_drive)
set_tests_properties(fuzz_config_drive PROPERTIES
                     ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

add_executable(od_fuzz_seed_config_drive seed_config_drive.c
    ${OD_FUZZ_CORE}/od_cfg_index.c ../host/od_log_test_stub.c)
target_link_libraries(od_fuzz_seed_config_drive od_shared)
target_include_directories(od_fuzz_seed_config_drive PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../host)
