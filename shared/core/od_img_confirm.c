/* od_img_confirm.c -- see od_img_confirm.h for the rule and why. */
#include "od_img_confirm.h"

#include <string.h>

void od_img_confirm_init(struct od_img_confirm *s, bool already_confirmed)
{
    memset(s, 0, sizeof *s);
    s->confirmed = already_confirmed;
    s->reason = already_confirmed ? OD_IMG_REASON_ALREADY : OD_IMG_REASON_NONE;
}

static enum od_img_confirm_action confirm(struct od_img_confirm *s, uint8_t reason)
{
    if (s->confirmed) {
        return OD_IMG_KEEP;
    }
    s->confirmed = true;
    s->reason = reason;
    return OD_IMG_CONFIRM;
}

enum od_img_confirm_action od_img_confirm_served(struct od_img_confirm *s)
{
    return confirm(s, OD_IMG_REASON_SERVED);
}

enum od_img_confirm_action od_img_confirm_link_closed(struct od_img_confirm *s,
                                                      enum od_health_action verdict,
                                                      uint8_t dead_links)
{
    if (dead_links != 0u || verdict == OD_HEALTH_ENTER_UPDATE) {
        s->dead_seen = true;
    }
    if (verdict == OD_HEALTH_ENTER_UPDATE && !s->confirmed) {
        s->reason = OD_IMG_REASON_DEAD;
        return OD_IMG_REVERT;
    }
    return OD_IMG_KEEP;
}

enum od_img_confirm_action od_img_confirm_poll(struct od_img_confirm *s, uint32_t uptime_ms)
{
    if (s->confirmed || s->dead_seen || uptime_ms < OD_IMG_CONFIRM_FALLBACK_MS) {
        return OD_IMG_KEEP;
    }
    return confirm(s, OD_IMG_REASON_FALLBACK);
}
