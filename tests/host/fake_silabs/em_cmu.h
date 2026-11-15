/* Minimal em_cmu surface for host tests that compile production BG22 sources. */
#ifndef OD_TEST_FAKE_SILABS_EM_CMU_H
#define OD_TEST_FAKE_SILABS_EM_CMU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    cmuClock_GPIO = 0
} CMU_Clock_TypeDef;

void CMU_ClockEnable(CMU_Clock_TypeDef clock, bool enable);

#ifdef __cplusplus
}
#endif

#endif /* OD_TEST_FAKE_SILABS_EM_CMU_H */
