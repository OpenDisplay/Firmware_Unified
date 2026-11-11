/* od_test_bench.h -- TEST-ONLY bench aids (src/od_test_bench.c). Every call compiles to the
 * release behaviour unless the bench build defines OD_TEST_BENCH (zephyr/CMakeLists.txt). */
#ifndef OD_TEST_BENCH_H
#define OD_TEST_BENCH_H

#include <stdbool.h>

#if defined(OD_TEST_BENCH) && OD_TEST_BENCH
/* Replaces main()'s boot_write_img_confirmed(): confirms (no revert switch) or arms the
 * no-touch revert. */
void od_test_bench_image_boot(void);
/* From the main loop: notices an SMP/console confirm, arms the watchdog backstop. */
void od_test_bench_tick(void);
/* True once an unconfirmed image has outlived its timer: the watchdog feed is withheld. */
bool od_test_bench_hold_feed(void);
#else
static inline void od_test_bench_tick(void) {}
static inline bool od_test_bench_hold_feed(void) { return false; }
#endif

#endif /* OD_TEST_BENCH_H */
