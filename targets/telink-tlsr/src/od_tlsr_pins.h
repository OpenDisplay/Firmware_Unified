/* Config pin bytes on this target: group * 8 + bit for PA0..PD7 (0..31), which the SDK names
 * (group << 8) | BIT(bit). 0xFF is "not present"; anything else is refused. Port E is not
 * usable: PE0..PE3 are the internal flash bus (MSDO, MCLK, MSCN, MSDI), and taking one as a
 * GPIO cuts the chip off from its own code -- a reset, or a reset loop if a stored config
 * names one. Include after od_tlsr_sdk.h. */
#ifndef OD_TLSR_PINS_H
#define OD_TLSR_PINS_H

#include <stdbool.h>
#include <stdint.h>

#define OD_TLSR_PIN_COUNT 32u

static inline bool od_tlsr_pin_valid(uint8_t p)
{
    return p < OD_TLSR_PIN_COUNT;
}

static inline GPIO_PinTypeDef od_tlsr_pin_sdk(uint8_t p)
{
    return (GPIO_PinTypeDef)(((unsigned)(p >> 3) << 8) | (1u << (p & 7u)));
}

/* The SAR ADC's GPIO inputs (the SDK's ADC_GPIO_tab): PB0..PB7, PC4, PC5. */
static inline bool od_tlsr_pin_adc(uint8_t p)
{
    return (p >= 8u && p <= 15u) || p == 20u || p == 21u;
}

#endif
