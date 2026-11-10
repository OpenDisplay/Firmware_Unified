/* od_display -- the config-driven display path, one implementation for every target.
 *
 * Everything a normal display operation needs between "the transfer layer has bytes" and "the
 * panel refreshed": resolve the panel and mode from the drive packets (0xAE-0xB1), apply the
 * mode's temp_policy, bring the panel up, stream the frame into controller RAM through the
 * sequence's image markers (planes, halves, slices), pick the refresh tail, sleep, power down.
 * Ordinary uploads (0x0070 / PIPE), partial updates (0x0076 / PIPE partial), multi-pass drive
 * transfers (0x0085), the boot screen and the image store all run through it.
 *
 * There is no panel table and no framebuffer. A config without drive packets drives nothing:
 * every entry point refuses with OD_DISPLAY_NEEDS_MIGRATION before touching the panel.
 *
 * The target keeps what is genuinely its own -- the rail, the bus, pin claiming, the die sensor,
 * the watchdog phase -- behind od_drive_app.h and od_display_app.h, and keeps its own transfer
 * glue (od_xfer_app_*), which routes panel bytes here. Flash diverts (CMD_FLASH_PRELOAD) never
 * reach this module.
 *
 * Mode conventions (docs/CONFIG_DRIVEN_DISPLAY.md): mode 0 is the plain full refresh; a mode
 * named "fast" supplies the refresh tail when END asks for FAST; a mode named "partial" drives
 * partial updates and is what makes the panel report partial support.
 */
#ifndef OD_DISPLAY_H
#define OD_DISPLAY_H

#include "od_color.h"
#include "od_span.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct od_config;
struct SecurityConfig;
struct od_boot_bufs;

typedef enum {
    OD_DISPLAY_OK = 0,
    OD_DISPLAY_NO_DISPLAY,        /* no DisplayConfig */
    OD_DISPLAY_NEEDS_MIGRATION,   /* the config carries no drive packets for the display */
    OD_DISPLAY_BAD_MODE,          /* mode missing or unusable for this operation */
    OD_DISPLAY_TEMP_RANGE,        /* temp_policy = REJECT and the reading is out of window */
    OD_DISPLAY_SEQ_UNSUPPORTED,   /* sequence uses an escape this target does not implement */
    OD_DISPLAY_PANEL,             /* rail, bus, BUSY or a malformed sequence */
} od_display_rc_t;

/* "3.x config-driven firmware, but this config has no drive data": the state a 2.x config is in
 * after an OTA. Shown to hosts by READ_IDENT and the upload NACKs. */
bool od_display_needs_migration(void);

/* True when a "partial" mode exists for the configured display. Loads the descriptor, so only
 * call it with no session open (od_xfer_app_panel_info runs after prepare_start). */
bool od_display_partial_available(void);

/* Ordinary full frame, mode 0. `geometry` is the wire layout of DisplayConfig 0x20's scheme. */
od_display_rc_t od_display_begin_full(const od_color_geometry_t *geometry);
/* Partial update of a byte-aligned rectangle: the wire carries the OLD plane then the NEW plane,
 * plane_bytes each, which the "partial" mode's two image markers take in that order. */
od_display_rc_t od_display_begin_partial(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                         uint32_t plane_bytes);
/* CMD_DRIVE_WRITE_START: the handler has selected the mode and opened pass 0. */
bool od_display_adopt_drive(const od_color_geometry_t *frame, uint8_t passes, bool compressed,
                            od_span_t tail);

bool od_display_write(uint32_t stream_offset, const uint8_t *data, size_t len);
/* Run the refresh, sleep the panel, take the rail down, end the session. refresh_mode is the
 * 0x0072 byte: FAST on a full upload runs the "fast" mode's tail when one is declared. */
bool od_display_refresh(uint8_t refresh_mode);
/* Idempotent. Sleeps and powers down a panel that is up. */
void od_display_abort(void);
bool od_display_active(void);

/* The boot screen through the same path: mode 0 (temp_policy REJECT degrades to advisory -- an
 * unreadable boot screen beats none), rendered row by row by od_boot_screen_render(). */
od_display_rc_t od_display_boot(const struct od_config *cfg, const struct SecurityConfig *sec,
                                const struct od_boot_bufs *bufs);

#ifdef __cplusplus
}
#endif

#endif /* OD_DISPLAY_H */
