/* BG22 link-facing policy: what may starve the BLE host, and what survives a config apply.
 *
 * Plain C with no vendor headers, so tests/host compiles it as-is. */
#ifndef OD_LINK_POLICY_H
#define OD_LINK_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A deferred panel job (the boot screen after a config write) blocks the superloop for a whole
 * refresh, 20-35 s, during which the Bluetooth host is not run: a peer on an open link gets no
 * ATT response and no notification for that long. So the job never starts while a link is
 * open, and connectable advertising is stopped for its duration so that no peer can open a
 * link into the stalled host -- a connection attempt then waits until the job is done and is
 * served normally.
 *
 * The stop and the run are separate loop passes: a connection the controller accepted before
 * the stop has its "opened" event delivered in between, and that cancels the run. */
enum od_panel_defer_step {
    OD_PANEL_DEFER_IDLE = 0,    /* nothing requested */
    OD_PANEL_DEFER_WAIT,        /* requested; a link is open or the transport is busy */
    OD_PANEL_DEFER_PAUSE_ADV,   /* stop connectable advertising now; run on a later pass */
    OD_PANEL_DEFER_RUN          /* advertising is stopped and no link is open: run the job,
                                 * then call od_panel_defer_done() and resume advertising */
};

struct od_panel_defer {
    bool pending;
    bool adv_paused;
};

void od_panel_defer_request(struct od_panel_defer *s);
enum od_panel_defer_step od_panel_defer_poll(struct od_panel_defer *s, bool link_open,
                                             bool transport_busy);
/* True when the caller paused advertising and must restart it now. */
bool od_panel_defer_done(struct od_panel_defer *s);
bool od_panel_defer_adv_paused(const struct od_panel_defer *s);

/* Whether the boot screen sends a second, blank image plane. Only the two-plane spot-colour
 * schemes have one; BWRY is a single packed 2-bpp plane, and sending a plane the drive
 * sequence does not declare makes the plane walk run off the end of the sequence. */
bool od_boot_has_blank_second_plane(uint8_t color_scheme);

#ifdef __cplusplus
}
#endif

#endif
