/* health_rollback_test.c -- od_health on a rollback target (Nordic MCUboot, ESP32 OTA rollback):
 * when an image confirms itself, when it reverts, and what happens before a deep sleep.
 * The first six cases are the hardware-proven Nordic rule (EN04, impl_en04-hw section 4),
 * unchanged in meaning from the od_img_confirm module they replace. */
#include "od_health.h"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) {                                                           \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static enum od_health_action link(struct od_health *h, uint32_t t, uint32_t dur, bool att)
{
    od_health_link_opened(h, t);
    if (att) {
        od_health_att_seen(h);
    }
    return od_health_link_closed(h, t + dur);
}

static void test_served_confirms_once(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    CHECK(od_health_poll(&h, 1000u) == OD_HEALTH_NONE);
    CHECK(od_health_served(&h) == OD_HEALTH_CONFIRM);
    CHECK(h.confirm_reason == OD_HEALTH_CONFIRM_SERVED);
    CHECK(od_health_served(&h) == OD_HEALTH_NONE);
    CHECK(od_health_poll(&h, OD_HEALTH_CONFIRM_FALLBACK_MS) == OD_HEALTH_NONE);
}

/* The BG22 stranding on a rollback target: links open, nothing is served, BlueZ gives up. */
static void test_dead_links_revert_unconfirmed(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    CHECK(link(&h, 30000u, 33000u, false) == OD_HEALTH_NONE);
    CHECK(link(&h, 70000u, 33000u, false) == OD_HEALTH_NONE);
    CHECK(link(&h, 110000u, 33000u, false) == OD_HEALTH_RECOVER);
    CHECK(h.reason == OD_HEALTH_REASON_DEAD_LINKS);
    CHECK(!h.confirmed);
}

static void test_dead_verdict_on_confirmed_image_keeps(void)
{
    struct od_health h;

    od_health_init_rollback(&h, true);
    CHECK(h.confirm_reason == OD_HEALTH_CONFIRM_ALREADY);
    (void)link(&h, 1000u, 33000u, false);
    (void)link(&h, 40000u, 33000u, false);
    CHECK(link(&h, 80000u, 33000u, false) == OD_HEALTH_NONE);
    CHECK(od_health_before_sleep(&h, 0u) == OD_HEALTH_NONE);
}

static void test_fallback_after_a_quiet_day(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    CHECK(od_health_poll(&h, OD_HEALTH_CONFIRM_FALLBACK_MS - 1u) == OD_HEALTH_NONE);
    CHECK(od_health_poll(&h, OD_HEALTH_CONFIRM_FALLBACK_MS) == OD_HEALTH_CONFIRM);
    CHECK(h.confirm_reason == OD_HEALTH_CONFIRM_FALLBACK);
}

/* Scanners that connect and drop (< 8 s) and healthy links do not block the fallback; one dead
 * link does, even if a later link is healthy-looking at the ATT level but never served. */
static void test_fallback_blocked_by_a_dead_link(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    CHECK(link(&h, 1000u, 2000u, false) == OD_HEALTH_NONE);    /* short: ignored */
    CHECK(link(&h, 5000u, 20000u, true) == OD_HEALTH_NONE);    /* ATT (MTU) but no write */
    CHECK(od_health_poll(&h, OD_HEALTH_CONFIRM_FALLBACK_MS) == OD_HEALTH_CONFIRM);

    od_health_init_rollback(&h, false);
    CHECK(link(&h, 1000u, 20000u, false) == OD_HEALTH_NONE);   /* one dead link */
    CHECK(link(&h, 30000u, 20000u, true) == OD_HEALTH_NONE);   /* healthy after */
    CHECK(od_health_poll(&h, OD_HEALTH_CONFIRM_FALLBACK_MS) == OD_HEALTH_NONE);
    CHECK(od_health_served(&h) == OD_HEALTH_CONFIRM);          /* evidence still confirms */
}

/* A served write between dead links confirms; later dead links then cannot revert. */
static void test_served_wins_over_later_dead_links(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    (void)link(&h, 1000u, 20000u, false);
    CHECK(od_health_served(&h) == OD_HEALTH_CONFIRM);
    (void)link(&h, 40000u, 20000u, false);
    (void)link(&h, 70000u, 20000u, false);
    CHECK(link(&h, 100000u, 20000u, false) == OD_HEALTH_NONE);
}

/* The stack's heap report on a no-ATT link reverts an unconfirmed image at once. */
static void test_stack_heap_failure_reverts_at_once(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    od_health_link_opened(&h, 1000u);
    od_health_stack_heap_failure(&h, 1u);
    CHECK(od_health_link_closed(&h, 1200u) == OD_HEALTH_RECOVER);
    CHECK(h.reason == OD_HEALTH_REASON_STACK_HEAP);
}

/* A failed confirm write is retried on the next evidence. */
static void test_confirm_failure_retries(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    CHECK(od_health_served(&h) == OD_HEALTH_CONFIRM);
    od_health_confirm_failed(&h);
    CHECK(!h.confirmed);
    CHECK(od_health_served(&h) == OD_HEALTH_CONFIRM);
}

/* Confirmed by SMP: no later action, no revert. */
static void test_external_confirm(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    od_health_confirmed_externally(&h);
    CHECK(h.confirmed && h.confirm_reason == OD_HEALTH_CONFIRM_EXTERNAL);
    CHECK(od_health_served(&h) == OD_HEALTH_NONE);
    (void)link(&h, 1000u, 20000u, false);
    (void)link(&h, 40000u, 20000u, false);
    CHECK(link(&h, 80000u, 20000u, false) == OD_HEALTH_NONE);
}

/* ESP32 deep sleep: hold while unconfirmed and young, then confirm (no objection) or revert. */
static void test_before_sleep(void)
{
    struct od_health h;

    od_health_init_rollback(&h, false);
    CHECK(od_health_before_sleep(&h, 0u) == OD_HEALTH_HOLD);
    CHECK(od_health_before_sleep(&h, OD_HEALTH_SLEEP_HOLD_MS - 1u) == OD_HEALTH_HOLD);
    CHECK(od_health_before_sleep(&h, OD_HEALTH_SLEEP_HOLD_MS) == OD_HEALTH_CONFIRM);
    CHECK(h.confirm_reason == OD_HEALTH_CONFIRM_SLEEP);
    CHECK(od_health_before_sleep(&h, OD_HEALTH_SLEEP_HOLD_MS) == OD_HEALTH_NONE);

    od_health_init_rollback(&h, false);
    (void)link(&h, 1000u, 20000u, false);                       /* one dead link */
    CHECK(od_health_before_sleep(&h, 60000u) == OD_HEALTH_HOLD);
    CHECK(od_health_before_sleep(&h, OD_HEALTH_SLEEP_HOLD_MS) == OD_HEALTH_RECOVER);
    CHECK(h.reason == OD_HEALTH_REASON_SLEEP && !h.confirmed);

    od_health_init_rollback(&h, false);
    CHECK(od_health_served(&h) == OD_HEALTH_CONFIRM);
    CHECK(od_health_before_sleep(&h, 0u) == OD_HEALTH_NONE);    /* confirmed: sleep freely */
}

/* An updater target (BG22) never confirms and never holds sleep. */
static void test_updater_kind_ignores_trust_calls(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    CHECK(od_health_served(&h) == OD_HEALTH_NONE);
    CHECK(od_health_poll(&h, OD_HEALTH_CONFIRM_FALLBACK_MS) == OD_HEALTH_NONE);
    CHECK(od_health_before_sleep(&h, 0u) == OD_HEALTH_NONE);
    /* served counts as ATT evidence: the link is healthy */
    od_health_link_opened(&h, 0u);
    (void)od_health_served(&h);
    CHECK(od_health_link_closed(&h, 30000u) == OD_HEALTH_NONE && h.dead_links == 0u);
}

int main(void)
{
    test_served_confirms_once();
    test_dead_links_revert_unconfirmed();
    test_dead_verdict_on_confirmed_image_keeps();
    test_fallback_after_a_quiet_day();
    test_fallback_blocked_by_a_dead_link();
    test_served_wins_over_later_dead_links();
    test_stack_heap_failure_reverts_at_once();
    test_confirm_failure_retries();
    test_external_confirm();
    test_before_sleep();
    test_updater_kind_ignores_trust_calls();
    if (failures != 0) {
        fprintf(stderr, "health_rollback: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("health_rollback: all passed\n");
    return EXIT_SUCCESS;
}
