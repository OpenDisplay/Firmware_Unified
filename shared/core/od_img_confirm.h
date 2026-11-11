/* od_img_confirm.h -- when a freshly swapped-in MCUboot image may confirm itself.
 *
 * THE FAILURE IT GUARDS: an image that boots, advertises and accepts links but whose GATT server
 * never answers (the BG22 stranding, docs/BG22_RAM_BUDGET.md). Confirmed at boot, such an image
 * can never be rolled back over the air. So an unconfirmed image confirms only on EVIDENCE that
 * it serves GATT, and a demonstrably dead one resets so MCUboot reverts it.
 *
 * THE RULE (one per event; the target acts on the returned action):
 *   SERVED     the application answered a GATT write from a peer (a request reached the
 *              OpenDisplay characteristic and was accepted)          -> CONFIRM
 *   link close od_health's verdict ENTER_UPDATE (a link the stack reported a heap failure on, or
 *              OD_HEALTH_DEAD_LINKS consecutive >= 8 s links with no ATT at all)
 *                                                                     -> REVERT (reset, unconfirmed)
 *              any dead link at all also disables the fallback below
 *   FALLBACK   OD_IMG_CONFIRM_FALLBACK_MS of uptime on this boot with no dead link seen
 *                                                                     -> CONFIRM
 * WHY THE FALLBACK, AND WHY 24 h: an updater client reconnects within seconds to minutes, so a
 * healthy image confirms by SERVED in normal use. A device nobody contacts after its update would
 * otherwise stay unconfirmed indefinitely, and the next unrelated reset (a battery swap months
 * later) would silently undo the update. After a day of uptime with no link that failed to carry
 * ATT, the image has given no evidence against itself; confirming then bounds that surprise. A
 * GATT-dead image that nobody tries to use for a day is confirmed too -- the accepted residual --
 * but the first three hosts that try within the day revert it.
 * An image that is already confirmed (a normal boot) does nothing here: REVERT has nothing to
 * revert to, so a dead verdict on a confirmed image is reported as KEEP.
 *
 * Pure logic, no HAL. CONTEXT: single loop/main task. */
#ifndef OD_IMG_CONFIRM_H
#define OD_IMG_CONFIRM_H

#include <stdbool.h>
#include <stdint.h>

#include "od_health.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OD_IMG_CONFIRM_FALLBACK_MS
#define OD_IMG_CONFIRM_FALLBACK_MS (24u * 3600u * 1000u)
#endif

enum od_img_confirm_action {
    OD_IMG_KEEP = 0,
    OD_IMG_CONFIRM,
    OD_IMG_REVERT
};

enum od_img_confirm_reason {
    OD_IMG_REASON_NONE = 0,
    OD_IMG_REASON_ALREADY,      /* confirmed before this boot */
    OD_IMG_REASON_SERVED,       /* a GATT write was served */
    OD_IMG_REASON_FALLBACK,     /* OD_IMG_CONFIRM_FALLBACK_MS with no dead link */
    OD_IMG_REASON_DEAD          /* od_health judged the image unable to serve GATT */
};

struct od_img_confirm {
    bool    confirmed;
    bool    dead_seen;   /* a link this boot carried no ATT for >= OD_HEALTH_DEAD_LINK_MS */
    uint8_t reason;      /* enum od_img_confirm_reason of the last CONFIRM / REVERT */
};

void od_img_confirm_init(struct od_img_confirm *s, bool already_confirmed);
/* A GATT write from a peer reached the application and was accepted. */
enum od_img_confirm_action od_img_confirm_served(struct od_img_confirm *s);
/* After od_health_link_closed(): its verdict, and h->dead_links after it. */
enum od_img_confirm_action od_img_confirm_link_closed(struct od_img_confirm *s,
                                                      enum od_health_action verdict,
                                                      uint8_t dead_links);
/* From the loop, any rate. */
enum od_img_confirm_action od_img_confirm_poll(struct od_img_confirm *s, uint32_t uptime_ms);

#ifdef __cplusplus
}
#endif

#endif /* OD_IMG_CONFIRM_H */
