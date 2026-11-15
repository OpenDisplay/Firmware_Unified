#ifndef OPENDISPLAY_DISPLAY_H
#define OPENDISPLAY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "od_color.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot screen with bounded retry. True when drawn or deliberately skipped (safe mode,
 * CLEAR_ON_BOOT, a config without drive packets). */
bool opendisplay_display_boot_apply(void);
/* After a config write: show the boot screen on the loop if it has never been shown. */
void opendisplay_display_request_boot(void);
/* Loop thread: the queued image-store slot, then a requested boot screen, once the link and the
 * transfer plane are idle. */
void opendisplay_display_service(void);
void opendisplay_display_park_pins(void);

#ifdef __cplusplus
}
#endif

#endif
