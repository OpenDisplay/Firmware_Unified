/* OD-side internals shared between this target's od_*.c files. Never included by SDK-side code. */
#ifndef OD_TLSR_H
#define OD_TLSR_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config.h"
#include "od_config_asm.h"

#ifndef OD_TLSR_VERSION_MAJOR
#define OD_TLSR_VERSION_MAJOR 1u
#endif
#ifndef OD_TLSR_VERSION_MINOR
#define OD_TLSR_VERSION_MINOR 0u
#endif
#ifndef OD_TLSR_VERSION_PATCH
#define OD_TLSR_VERSION_PATCH 0u
#endif
#ifndef OD_TLSR_BUILD_ID
#define OD_TLSR_BUILD_ID "tlsr825x-diag2"
#endif

struct od_config_asm *od_tlsr_config_assembler(void);
const struct od_config *od_tlsr_config(void);
/* LED instance `instance` of the live config, or NULL. Mutable: its reserved[] holds the running
 * LED pattern, whose mode nibble a config reload clears -- which is what stops a pattern. */
struct LedConfig *od_tlsr_led(uint8_t instance);
/* SystemConfig.pwr_pin_2 when it is the second panel supply switch (dual-controller panels on a
 * board without a power latch), else 0xFF. */
uint8_t od_tlsr_panel_pwr2(const struct od_config *cfg);
bool od_tlsr_config_save(const uint8_t *data, uint32_t len);
bool od_tlsr_config_load(uint8_t *out, uint32_t *len);
bool od_tlsr_config_clear(void);
void od_tlsr_config_reload(void);

void od_tlsr_publish_msd(void);
void od_tlsr_copy_msd(uint8_t out[16]);

/* Panel-controller temperature, read whenever the panel is powered (od_xfer_tlsr.c). */
void od_tlsr_set_temperature(int8_t celsius);

uint32_t od_tlsr_link_tag(void);
bool od_tlsr_notify_subscribed(void);

#endif /* OD_TLSR_H */
