#include "od_link_policy.h"

void od_panel_defer_request(struct od_panel_defer *s)
{
    s->pending = true;
}

enum od_panel_defer_step od_panel_defer_poll(struct od_panel_defer *s, bool link_open,
                                             bool transport_busy)
{
    if (!s->pending) {
        return OD_PANEL_DEFER_IDLE;
    }
    if (link_open || transport_busy) {
        /* A link that opened after the pause: the controller stopped advertising for it, and
         * the close handler restarts advertising, so the pause is simply forgotten. */
        s->adv_paused = false;
        return OD_PANEL_DEFER_WAIT;
    }
    if (!s->adv_paused) {
        s->adv_paused = true;
        return OD_PANEL_DEFER_PAUSE_ADV;
    }
    s->pending = false;
    return OD_PANEL_DEFER_RUN;
}

bool od_panel_defer_done(struct od_panel_defer *s)
{
    const bool resume = s->adv_paused;
    s->adv_paused = false;
    return resume;
}

bool od_panel_defer_adv_paused(const struct od_panel_defer *s)
{
    return s->adv_paused;
}

bool od_boot_has_blank_second_plane(uint8_t color_scheme)
{
    return color_scheme == OD_COLOR_SCHEME_BWR || color_scheme == OD_COLOR_SCHEME_BWY;
}
