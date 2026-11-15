/* Target-internal interfaces between this target's own files. */
#ifndef OD_TLSR_TARGET_H
#define OD_TLSR_TARGET_H

#include <stdbool.h>
#include <stdint.h>

struct od_config;

/* od_tlsr_cmd.c: the parsed config and the one config workspace. */
bool od_tlsr_config_load(void);
const struct od_config *od_tlsr_config(void);
bool od_tlsr_config_loaded(void);
void od_tlsr_config_reload(void);
void od_tlsr_cmd_reset(void);
uint8_t *od_tlsr_config_workspace(uint32_t *cap);
bool od_tlsr_config_assembling(void);

/* od_tlsr_hal.c */
void od_tlsr_hal_boot(void);
/* Unplanned resets (watchdog, reset pin) since power was applied, saturating at 7. */
uint8_t od_tlsr_hal_unplanned(void);
float od_tlsr_chip_temperature_c(void);
uint16_t od_tlsr_battery_mv(void);
bool od_tlsr_reboot_flag(void);
void od_tlsr_mac(uint8_t mac[6]);
void od_tlsr_feed_watchdog(void);

/* od_tlsr_display.c */
void od_tlsr_display_init(void);
void od_tlsr_display_request_boot(void);
void od_tlsr_display_service(void);
/* Nothing for the display to do: no boot screen pending, no raw session, no refresh running. */
bool od_tlsr_display_idle(void);
/* After a deep-retention wake: put back the pin state the idle panel had. RAM survives the
 * sleep; the digital GPIO registers do not. */
void od_tlsr_display_retention_wake(void);
#if OD_TLSR_FAULT_TEST
bool od_tlsr_fault_armed(void);
#endif
/* Glass temperature from the panel controller, read at the last panel-up; false when none
 * is under an hour old. */
bool od_tlsr_display_temperature(int8_t *c);

#endif /* OD_TLSR_TARGET_H */
