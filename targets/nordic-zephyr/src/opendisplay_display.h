#ifndef OPENDISPLAY_DISPLAY_H
#define OPENDISPLAY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "od_color.h"

#ifdef __cplusplus
extern "C" {
#endif

bool opendisplay_display_boot_apply(void);
void opendisplay_display_park_pins(void);

#ifdef __cplusplus
}
#endif

#endif
