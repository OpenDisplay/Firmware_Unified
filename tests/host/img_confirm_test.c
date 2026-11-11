/* img_confirm_test.c -- od_img_confirm: when an MCUboot image confirms itself, when it reverts.
 * Driven through the real od_health so the dead-link arithmetic is the shipped one. */
#include "od_img_confirm.h"

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

static enum od_img_confirm_action link(struct od_health *h, struct od_img_confirm *s, uint32_t t,
                                       uint32_t dur, bool att)
{
    enum od_health_action v;

    od_health_link_opened(h, t);
    if (att) {
        od_health_att_seen(h);
    }
    v = od_health_link_closed(h, t + dur);
    return od_img_confirm_link_closed(s, v, h->dead_links);
}

static void test_served_confirms_once(void)
{
    struct od_img_confirm s;

    od_img_confirm_init(&s, false);
    CHECK(od_img_confirm_poll(&s, 1000u) == OD_IMG_KEEP);
    CHECK(od_img_confirm_served(&s) == OD_IMG_CONFIRM);
    CHECK(s.reason == OD_IMG_REASON_SERVED);
    CHECK(od_img_confirm_served(&s) == OD_IMG_KEEP);
    CHECK(od_img_confirm_poll(&s, OD_IMG_CONFIRM_FALLBACK_MS) == OD_IMG_KEEP);
}

/* The BG22 stranding on Nordic: links open, nothing is served, BlueZ gives up after ~33 s. */
static void test_dead_links_revert_unconfirmed(void)
{
    struct od_health h;
    struct od_img_confirm s;

    od_health_init(&h, 0u);
    od_img_confirm_init(&s, false);
    CHECK(link(&h, &s, 30000u, 33000u, false) == OD_IMG_KEEP);
    CHECK(link(&h, &s, 70000u, 33000u, false) == OD_IMG_KEEP);
    CHECK(link(&h, &s, 110000u, 33000u, false) == OD_IMG_REVERT);
    CHECK(s.reason == OD_IMG_REASON_DEAD);
    CHECK(!s.confirmed);
}

static void test_dead_verdict_on_confirmed_image_keeps(void)
{
    struct od_health h;
    struct od_img_confirm s;

    od_health_init(&h, 0u);
    od_img_confirm_init(&s, true);
    CHECK(s.reason == OD_IMG_REASON_ALREADY);
    (void)link(&h, &s, 1000u, 33000u, false);
    (void)link(&h, &s, 40000u, 33000u, false);
    CHECK(link(&h, &s, 80000u, 33000u, false) == OD_IMG_KEEP);
}

static void test_fallback_after_a_quiet_day(void)
{
    struct od_img_confirm s;

    od_img_confirm_init(&s, false);
    CHECK(od_img_confirm_poll(&s, OD_IMG_CONFIRM_FALLBACK_MS - 1u) == OD_IMG_KEEP);
    CHECK(od_img_confirm_poll(&s, OD_IMG_CONFIRM_FALLBACK_MS) == OD_IMG_CONFIRM);
    CHECK(s.reason == OD_IMG_REASON_FALLBACK);
}

/* Scanners that connect and drop (< 8 s) and healthy links do not block the fallback; one dead
 * link does, even if a later link is healthy-looking at the ATT level but never served. */
static void test_fallback_blocked_by_a_dead_link(void)
{
    struct od_health h;
    struct od_img_confirm s;

    od_health_init(&h, 0u);
    od_img_confirm_init(&s, false);
    CHECK(link(&h, &s, 1000u, 2000u, false) == OD_IMG_KEEP);    /* short: ignored */
    CHECK(link(&h, &s, 5000u, 20000u, true) == OD_IMG_KEEP);    /* ATT (MTU) but no write */
    CHECK(od_img_confirm_poll(&s, OD_IMG_CONFIRM_FALLBACK_MS) == OD_IMG_CONFIRM);

    od_health_init(&h, 0u);
    od_img_confirm_init(&s, false);
    CHECK(link(&h, &s, 1000u, 20000u, false) == OD_IMG_KEEP);   /* one dead link */
    CHECK(link(&h, &s, 30000u, 20000u, true) == OD_IMG_KEEP);   /* healthy after */
    CHECK(od_img_confirm_poll(&s, OD_IMG_CONFIRM_FALLBACK_MS) == OD_IMG_KEEP);
    CHECK(od_img_confirm_served(&s) == OD_IMG_CONFIRM);          /* evidence still confirms */
}

/* A served write between dead links confirms; later dead links then cannot revert. */
static void test_served_wins_over_later_dead_links(void)
{
    struct od_health h;
    struct od_img_confirm s;

    od_health_init(&h, 0u);
    od_img_confirm_init(&s, false);
    (void)link(&h, &s, 1000u, 20000u, false);
    CHECK(od_img_confirm_served(&s) == OD_IMG_CONFIRM);
    (void)link(&h, &s, 40000u, 20000u, false);
    (void)link(&h, &s, 70000u, 20000u, false);
    CHECK(link(&h, &s, 100000u, 20000u, false) == OD_IMG_KEEP);
}

int main(void)
{
    test_served_confirms_once();
    test_dead_links_revert_unconfirmed();
    test_dead_verdict_on_confirmed_image_keeps();
    test_fallback_after_a_quiet_day();
    test_fallback_blocked_by_a_dead_link();
    test_served_wins_over_later_dead_links();
    if (failures != 0) {
        fprintf(stderr, "img_confirm: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("img_confirm: all passed\n");
    return EXIT_SUCCESS;
}
