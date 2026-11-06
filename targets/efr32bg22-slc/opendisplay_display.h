#ifndef OPENDISPLAY_DISPLAY_H
#define OPENDISPLAY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void opendisplay_display_abort(void);
void opendisplay_display_boot_apply(void);
void opendisplay_display_park_pins(void);
void opendisplay_display_power_off(void);

/* Runs a queued display-from-slot refresh. Called from the superloop, because the
 * refresh is seconds long and must not sit inside a command handler. */
void opendisplay_display_service(void);

#ifdef __cplusplus
}
#endif

#endif
