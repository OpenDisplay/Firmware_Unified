/* od_adv_watch.c -- see od_adv_watch.h. */
#include "od_adv_watch.h"

bool od_adv_watch_is_down(const struct od_adv_control *s)
{
    if (!s->stack_ready) {
        return false;
    }
    if (s->faulted) {
        return true;
    }
    return s->desired && !s->active && s->connection_count == 0u;
}

bool od_adv_watch_step(struct od_adv_watch *w, const struct od_adv_control *s, uint32_t now_ms)
{
    if (!od_adv_watch_is_down(s)) {
        w->down = false;
        return false;
    }
    if (!w->down) {
        w->down = true;
        w->down_since = now_ms;
        return false;
    }
    return (uint32_t)(now_ms - w->down_since) >= OD_ADV_WATCH_RESET_MS;
}
