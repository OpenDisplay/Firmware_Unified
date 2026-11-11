/* health_test.c -- od_health: when an image is judged unable to serve GATT.
 *
 * Built twice: with the power-cycle escape off (the default every target ships) and with it at
 * 2 short boots (OD_HEALTH_TEST_PATTERN=1). */
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

/* One link: open at t, optional ATT, optional heap failure, close at t + dur. */
static enum od_health_action link(struct od_health *h, uint32_t t, uint32_t dur, bool att,
                                  uint32_t heap_fail)
{
    od_health_link_opened(h, t);
    if (heap_fail != 0u) {
        od_health_stack_heap_failure(h, heap_fail);
    }
    if (att) {
        od_health_att_seen(h);
    }
    return od_health_link_closed(h, t + dur);
}

/* The stranding: every link opens, no ATT ever, BlueZ gives up after ~33 s. */
static void test_dead_links_force_update_on_the_third(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    CHECK(link(&h, 1000u, 33000u, false, 0u) == OD_HEALTH_NONE);
    CHECK(link(&h, 40000u, 33000u, false, 0u) == OD_HEALTH_NONE);
    CHECK(h.dead_links == 2u);
    CHECK(link(&h, 80000u, 33000u, false, 0u) == OD_HEALTH_ENTER_UPDATE);
    CHECK(h.reason == OD_HEALTH_REASON_DEAD_LINKS && h.forced == 1u);
}

/* The stack's own report on a link with no ATT, before anything was served: immediate. */
static void test_stack_heap_failure_forces_update_at_first_close(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    CHECK(link(&h, 1000u, 200u, false, 1u) == OD_HEALTH_ENTER_UPDATE);
    CHECK(h.reason == OD_HEALTH_REASON_STACK_HEAP && h.heap_failures == 1u);
}

/* A heap failure on a link that still carried ATT is not this failure. */
static void test_heap_failure_with_att_is_healthy(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    CHECK(link(&h, 1000u, 20000u, true, 3u) == OD_HEALTH_NONE);
    CHECK(h.dead_links == 0u && h.heap_failures == 3u);
}

/* After this boot has served GATT, a heap-failure link counts toward the threshold only. */
static void test_heap_failure_after_proven_needs_the_threshold(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    CHECK(link(&h, 0u, 5000u, true, 0u) == OD_HEALTH_NONE);
    CHECK(link(&h, 10000u, 100u, false, 1u) == OD_HEALTH_NONE);
    CHECK(link(&h, 20000u, 100u, false, 1u) == OD_HEALTH_NONE);
    CHECK(link(&h, 30000u, 100u, false, 1u) == OD_HEALTH_ENTER_UPDATE);
    CHECK(h.reason == OD_HEALTH_REASON_DEAD_LINKS);
}

/* A heap failure outside any link (boot) is recorded but does not poison the next link. */
static void test_heap_failure_without_link_is_diagnostic(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    od_health_stack_heap_failure(&h, 2u);
    CHECK(h.heap_failures == 2u);
    CHECK(link(&h, 1000u, 100u, false, 0u) == OD_HEALTH_NONE);
    CHECK(h.dead_links == 0u);
}

/* A busy radio environment: scanners connect and drop, phones come and go. Never forced. */
static void test_busy_environment_never_forces(void)
{
    struct od_health h;
    uint32_t t = 0u;
    int i;

    od_health_init(&h, 0u);
    for (i = 0; i < 1000; ++i) {
        /* short no-ATT links, the threshold length minus one, interleaved with healthy ones */
        CHECK(link(&h, t, (i % 7 == 0) ? OD_HEALTH_DEAD_LINK_MS - 1u : 300u, false, 0u)
              == OD_HEALTH_NONE);
        t += 10000u;
        if (i % 2 == 0) {
            CHECK(link(&h, t, 2000u, false, 0u) == OD_HEALTH_NONE);   /* short: ignored */
        }
        t += 10000u;
    }
    /* two dead links, then a healthy one, repeatedly: the count never reaches three */
    for (i = 0; i < 100; ++i) {
        CHECK(link(&h, t, 30000u, false, 0u) == OD_HEALTH_NONE);
        CHECK(link(&h, t + 40000u, 30000u, false, 0u) == OD_HEALTH_NONE);
        CHECK(link(&h, t + 80000u, 1000u, true, 0u) == OD_HEALTH_NONE);
        t += 120000u;
    }
    CHECK(h.forced == 0u && h.dead_links == 0u);
}

/* At the threshold length a link is dead; one ms less it is not. */
static void test_dead_link_boundary(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    CHECK(link(&h, 0u, OD_HEALTH_DEAD_LINK_MS - 1u, false, 0u) == OD_HEALTH_NONE);
    CHECK(h.dead_links == 0u);
    CHECK(link(&h, 0u, OD_HEALTH_DEAD_LINK_MS, false, 0u) == OD_HEALTH_NONE);
    CHECK(h.dead_links == 1u);
}

/* Uptime wrap inside a link. */
static void test_uptime_wrap(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    CHECK(link(&h, 0xFFFFF000u, OD_HEALTH_DEAD_LINK_MS, false, 0u) == OD_HEALTH_NONE);
    CHECK(h.dead_links == 1u);
}

/* Bounded per power-on: the retained count stops it after OD_HEALTH_FORCED_MAX. */
static void test_forced_entries_are_bounded(void)
{
    struct od_health h;
    unsigned boot;
    unsigned entered = 0u;
    uint8_t retained = 0u;

    for (boot = 0u; boot < 10u; ++boot) {
        int i;

        od_health_init(&h, retained);
        for (i = 0; i < 5; ++i) {
            if (link(&h, (uint32_t)i * 60000u, 33000u, false, 1u) == OD_HEALTH_ENTER_UPDATE) {
                entered++;
                break;
            }
        }
        retained = h.forced;
    }
    CHECK(entered == OD_HEALTH_FORCED_MAX);
    od_health_init(&h, 0u);                      /* a power cycle: allowed again */
    CHECK(link(&h, 0u, 100u, false, 1u) == OD_HEALTH_ENTER_UPDATE);
}

/* Overlapping links are not judged, and the counters stay consistent. */
static void test_overlap_not_judged(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
    od_health_link_opened(&h, 0u);
    od_health_link_opened(&h, 100u);
    od_health_stack_heap_failure(&h, 1u);
    CHECK(od_health_link_closed(&h, 40000u) == OD_HEALTH_NONE);
    CHECK(od_health_link_closed(&h, 50000u) == OD_HEALTH_NONE);
    CHECK(h.open_links == 0u && h.dead_links == 0u && h.links == 0u);
    CHECK(od_health_link_closed(&h, 60000u) == OD_HEALTH_NONE);   /* stray close */
    CHECK(h.open_links == 0u);
    /* the next single link is judged normally */
    CHECK(link(&h, 70000u, 100u, false, 1u) == OD_HEALTH_ENTER_UPDATE);
}

static void test_pattern(void)
{
    struct od_health h;

    od_health_init(&h, 0u);
#if OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS == 0u
    CHECK(!od_health_update_pattern(&h, 2u, true));
    CHECK(!od_health_update_pattern(&h, 0u, true));
    CHECK(h.forced == 0u);
#else
    CHECK(!od_health_update_pattern(&h, OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS, false)); /* claimed */
    CHECK(!od_health_update_pattern(&h, OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS - 1u, true));
    CHECK(!od_health_update_pattern(&h, OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS + 1u, true));
    CHECK(od_health_update_pattern(&h, OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS, true));
    CHECK(h.reason == OD_HEALTH_REASON_PATTERN && h.forced == 1u);
    od_health_init(&h, OD_HEALTH_FORCED_MAX);
    CHECK(!od_health_update_pattern(&h, OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS, true));
#endif
}

int main(void)
{
    test_dead_links_force_update_on_the_third();
    test_stack_heap_failure_forces_update_at_first_close();
    test_heap_failure_with_att_is_healthy();
    test_heap_failure_after_proven_needs_the_threshold();
    test_heap_failure_without_link_is_diagnostic();
    test_busy_environment_never_forces();
    test_dead_link_boundary();
    test_uptime_wrap();
    test_forced_entries_are_bounded();
    test_overlap_not_judged();
    test_pattern();
    if (failures != 0) {
        fprintf(stderr, "health_test: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("health_test: ok (pattern=%u)\n", (unsigned)OD_HEALTH_UPDATE_PATTERN_SHORT_BOOTS);
    return EXIT_SUCCESS;
}
