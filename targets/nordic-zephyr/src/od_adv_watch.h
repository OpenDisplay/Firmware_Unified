/* od_adv_watch.h -- bounded recovery when advertising stays down (pure C, host-tested).
 *
 * od_adv_control latches `faulted` on any HAL error it cannot classify as a retry, and only a new
 * stack generation clears it. This target enables the stack once, so a latched fault -- or an
 * advertiser that never comes back for any other reason while nobody is connected -- would leave
 * the unit invisible until its battery is pulled, with the loop still feeding the watchdog.
 *
 * DOWN means: the stack is ready, and either the controller is faulted, or advertising is wanted,
 * not running and no peer is connected. Normal stop/program/start cycles and post-disconnect
 * RETRYs resolve in milliseconds; OD_ADV_WATCH_RESET_MS of continuous DOWN is far outside them.
 * The verdict is a reset: it re-runs bt_enable, and on an unconfirmed MCUboot image it reverts. */
#ifndef OD_ADV_WATCH_H
#define OD_ADV_WATCH_H

#include <stdbool.h>
#include <stdint.h>

#include "od_adv_control.h"

#ifndef OD_ADV_WATCH_RESET_MS
#define OD_ADV_WATCH_RESET_MS 60000u
#endif

struct od_adv_watch {
    bool     down;        /* DOWN at the last step */
    uint32_t down_since;  /* uptime at which the current DOWN stretch began */
};

bool od_adv_watch_is_down(const struct od_adv_control *s);

/* Once per loop pass, any rate. True once DOWN has lasted OD_ADV_WATCH_RESET_MS: reset now. Any
 * pass that is not DOWN restarts the count. Wrap-safe. */
bool od_adv_watch_step(struct od_adv_watch *w, const struct od_adv_control *s, uint32_t now_ms);

#endif /* OD_ADV_WATCH_H */
