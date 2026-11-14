/* The TLSR825x LED adapter (od_led_tlsr.c) as the command handlers and main loop see it. */
#ifndef OD_LED_TLSR_H
#define OD_LED_TLSR_H

#include <stdbool.h>
#include <stdint.h>

/* LED_ACTIVATE: `rest` is the payload after the instance byte. 0 = accepted (including the
 * deployed "mode is not run" case, which leaves the LEDs off), 2 = no such LED instance. */
int od_led_tlsr_activate(uint8_t instance, const uint8_t *rest, uint16_t rest_len);

/* LED_STOP: 0 = stopped or idle, 2 = `instance_given` and another instance owns the run. */
int od_led_tlsr_stop(uint8_t instance, bool instance_given);

/* Main loop: advance a running pattern when its next step is due. */
void od_led_tlsr_poll(void);

/* A short green blink after boot, when the config has an LED with a green pin. */
void od_led_tlsr_boot_blink(void);

#endif /* OD_LED_TLSR_H */
