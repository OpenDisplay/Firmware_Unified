/* Minimal em_system surface for host tests that compile production BG22 sources. */
#ifndef OD_TEST_FAKE_SILABS_EM_SYSTEM_H
#define OD_TEST_FAKE_SILABS_EM_SYSTEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t SYSTEM_GetUnique(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_TEST_FAKE_SILABS_EM_SYSTEM_H */
