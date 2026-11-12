/* od_test_bench.h -- TEST-ONLY bench aids (src/od_test_bench.c). Every call compiles to the
 * release behaviour unless the bench build defines OD_SEC2_BENCH_BUILD (zephyr/CMakeLists.txt). */
#ifndef OD_TEST_BENCH_H
#define OD_TEST_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#if defined(OD_SEC2_BENCH_BUILD) && OD_SEC2_BENCH_BUILD
/* With OD_SEC2_TEST_REVERT_S: arms the no-touch revert timer on an unconfirmed image (the
 * evidence rule in od_health_app.c still confirms it on a served write). */
void od_test_bench_image_boot(void);
/* From the main loop: notices an SMP/console confirm, arms the watchdog backstop. */
void od_test_bench_tick(void);
/* True once an unconfirmed image has outlived its timer: the watchdog feed is withheld. */
bool od_test_bench_hold_feed(void);
/* Bench only: a faked reset cause for this boot ('od por' / 'od pin'); false if none. */
bool od_test_bench_take_fake_cause(uint32_t *cause);
#else
static inline bool od_test_bench_take_fake_cause(uint32_t *cause) { (void)cause; return false; }
static inline void od_test_bench_tick(void) {}
static inline bool od_test_bench_hold_feed(void) { return false; }
#endif

#endif /* OD_TEST_BENCH_H */
