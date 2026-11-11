/* od_health.c -- see od_health.h. */

#include "od_health.h"

#include <string.h>

#if OD_HEALTH_DEAD_LINKS == 0u || OD_HEALTH_DEAD_LINKS > 255u
#error "OD_HEALTH_DEAD_LINKS must be 1..255"
#endif

static enum od_health_action force(struct od_health *h, enum od_health_reason why)
{
    if (h->forced >= OD_HEALTH_FORCED_MAX) {
        return OD_HEALTH_NONE;
    }
    h->forced++;
    h->reason = (uint8_t)why;
    return OD_HEALTH_ENTER_UPDATE;
}

void od_health_init(struct od_health *h, uint8_t forced_so_far)
{
    memset(h, 0, sizeof *h);
    h->forced = forced_so_far;
}

void od_health_link_opened(struct od_health *h, uint32_t now_ms)
{
    if (h->open_links < 255u) {
        h->open_links++;
    }
    if (h->open_links > 1u) {
        h->overlapped = true;
        return;
    }
    h->overlapped = false;
    h->link_open_ms = now_ms;
    h->link_att = false;
    h->link_heap_fail = false;
}

void od_health_att_seen(struct od_health *h)
{
    h->link_att = true;
    h->gatt_proven = true;
}

void od_health_stack_heap_failure(struct od_health *h, uint32_t n)
{
    if (n == 0u) {
        return;
    }
    h->heap_failures = (h->heap_failures > UINT32_MAX - n) ? UINT32_MAX : h->heap_failures + n;
    if (h->open_links > 0u) {
        h->link_heap_fail = true;
    }
}

enum od_health_action od_health_link_closed(struct od_health *h, uint32_t now_ms)
{
    bool judged;
    bool long_enough;

    if (h->open_links == 0u) {
        return OD_HEALTH_NONE;          /* a close with no open: nothing to judge */
    }
    h->open_links--;
    judged = !h->overlapped;
    if (h->open_links > 0u) {
        return OD_HEALTH_NONE;          /* the other link's close is judged as overlapped */
    }
    h->overlapped = false;
    if (!judged) {
        return OD_HEALTH_NONE;
    }
    if (h->links < UINT16_MAX) {
        h->links++;
    }
    if (h->link_att) {
        h->dead_links = 0u;
        return OD_HEALTH_NONE;
    }
    long_enough = (uint32_t)(now_ms - h->link_open_ms) >= OD_HEALTH_DEAD_LINK_MS;
    if (!h->link_heap_fail && !long_enough) {
        return OD_HEALTH_NONE;
    }
    if (h->dead_links < 255u) {
        h->dead_links++;
    }
    if (h->link_heap_fail && !h->gatt_proven) {
        return force(h, OD_HEALTH_REASON_STACK_HEAP);
    }
    if (h->dead_links >= OD_HEALTH_DEAD_LINKS) {
        return force(h, OD_HEALTH_REASON_DEAD_LINKS);
    }
    return OD_HEALTH_NONE;
}

bool od_health_update_pattern(struct od_health *h, uint8_t short_boots, bool unclaimed)
{
#if OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS == 0u
    (void)h;
    (void)short_boots;
    (void)unclaimed;
    return false;
#else
    if (!unclaimed || short_boots != OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS) {
        return false;
    }
    return force(h, OD_HEALTH_REASON_PATTERN) == OD_HEALTH_ENTER_UPDATE;
#endif
}
