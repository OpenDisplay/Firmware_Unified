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

struct DisplayConfig;

const struct DisplayConfig *opendisplay_display_config(void);
void opendisplay_display_abort(void);
/* Rail, SPI and panel pins for a CMD_DRIVE_WRITE_START session. */
bool opendisplay_display_drive_panel_up(const struct DisplayConfig *d);
void opendisplay_display_drive_panel_down(void);
bool opendisplay_display_drive_adopt(const od_color_geometry_t *geometry, uint8_t passes,
                                     bool compressed, const uint8_t *payload,
                                     uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
