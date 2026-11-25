/* CMD_DRIVE_WRITE_START (0x0085) flag ETAG (protocol 2.5): the check both 0x0085 handlers run
 * before any panel I/O (shared/core/od_cmd_drive.c and targets/efr32bg22-slc/od_cmd_drive.c).
 *
 * A host that sends only changed pixels (hold codes, 0xAE tail hold_mask) must know the glass
 * still shows its prior frame. It names that frame's etag here; the device keeps the etag of
 * what it shows (set by 0x0072 END's new_etag, cleared by every other draw and at boot). A
 * mismatch is refused, so a reboot, a boot or claim screen or another host's write can never
 * turn a partial into garbage.
 */
#ifndef OD_DRIVE_ETAG_H
#define OD_DRIVE_ETAG_H

#include "od_caps.h"
#include "od_span.h"
#include "od_xfer_app.h"
#include "opendisplay_protocol.h"

#include <stdint.h>

#if OD_CAP_DRIVE_WRITE

/* [old_etag:4 BE] right after the 5-byte header when flags carry ETAG. */
#define OD_DRIVE_ETAG_LEN 4u

/* Consumes old_etag from *tail when flags carry ETAG. Returns 0, OD_ERR_DRIVE_BAD_ARGS for a
 * short body, or OD_ERR_DRIVE_ETAG_MISMATCH with the displayed etag cleared. old_etag 0 claims
 * nothing and always passes. */
static inline uint8_t od_drive_etag_check(uint8_t flags, od_span_t *tail)
{
    uint32_t old_etag;

    if ((flags & DRIVE_WRITE_FLAG_ETAG) == 0u) {
        return 0u;
    }
    if (tail->p == NULL || tail->n < OD_DRIVE_ETAG_LEN) {
        return OD_ERR_DRIVE_BAD_ARGS;
    }
    old_etag = ((uint32_t)tail->p[0] << 24) | ((uint32_t)tail->p[1] << 16)
             | ((uint32_t)tail->p[2] << 8) | (uint32_t)tail->p[3];
    *tail = od_span_drop(*tail, OD_DRIVE_ETAG_LEN);
    if (old_etag != 0u && old_etag != od_xfer_app_displayed_etag()) {
        od_xfer_app_set_displayed_etag(0u);
        return OD_ERR_DRIVE_ETAG_MISMATCH;
    }
    return 0u;
}

#endif /* OD_CAP_DRIVE_WRITE */
#endif /* OD_DRIVE_ETAG_H */
