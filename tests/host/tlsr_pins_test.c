/* tlsr_pins_test.c -- the Telink config pin encoding (targets/telink-tlsr/src/od_tlsr_pins.h):
 * PA0..PD7 except PA7 (SWS) are usable, port E (the flash bus) and everything past it are not.
 * Every config-driven pin -- panel, rail, reset, raw GPIO -- goes through od_tlsr_pin_valid. */
typedef unsigned GPIO_PinTypeDef; /* the SDK's enum, for od_tlsr_pin_sdk */

#include "od_tlsr_pins.h"

#include <stdio.h>

static unsigned g_checks, g_failures;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            ++g_failures;                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                  \
        }                                                                       \
    } while (0)

int main(void)
{
    unsigned p;

    for (p = 0u; p < 256u; ++p) {
        const bool want = p < 32u && p != 7u;
        CHECK(od_tlsr_pin_valid((uint8_t)p) == want);
    }
    CHECK(od_tlsr_pin_sdk(0u) == 0x001u);  /* PA0 */
    CHECK(od_tlsr_pin_sdk(13u) == 0x120u); /* PB5 */
    CHECK(od_tlsr_pin_sdk(31u) == 0x380u); /* PD7 */
    CHECK(!od_tlsr_pin_adc(7u) && od_tlsr_pin_adc(8u) && od_tlsr_pin_adc(21u));
    printf("tlsr_pins: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
