/* nordic_adv_watch_test.c -- H2: a Nordic unit whose advertiser stays down (latched fault, or
 * wanted-but-not-running with nobody connected) resets after a bounded wait instead of staying
 * invisible; transient stops and connections never trigger it. */
#include "od_adv_watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static struct od_adv_control running(void)
{
    struct od_adv_control s;

    memset(&s, 0, sizeof s);
    s.stack_ready = true;
    s.desired = true;
    s.active = true;
    return s;
}

static void test_latched_fault_resets_after_bound(void)
{
    struct od_adv_watch w = {0};
    struct od_adv_control s = running();

    CHECK(!od_adv_watch_step(&w, &s, 1000u));
    s.active = false;
    s.faulted = true;
    CHECK(!od_adv_watch_step(&w, &s, 2000u));
    CHECK(!od_adv_watch_step(&w, &s, 2000u + OD_ADV_WATCH_RESET_MS - 1u));
    CHECK(od_adv_watch_step(&w, &s, 2000u + OD_ADV_WATCH_RESET_MS));
}

/* the latched fault is still DOWN while a stale connection count says someone is connected */
static void test_fault_counts_even_with_connection(void)
{
    struct od_adv_watch w = {0};
    struct od_adv_control s = running();

    s.faulted = true;
    s.connection_count = 1u;
    CHECK(!od_adv_watch_step(&w, &s, 0u));
    CHECK(od_adv_watch_step(&w, &s, OD_ADV_WATCH_RESET_MS));
}

static void test_connection_and_transients_do_not_reset(void)
{
    struct od_adv_watch w = {0};
    struct od_adv_control s = running();
    uint32_t t;

    /* connected for ten minutes: not down */
    s.active = false;
    s.connection_count = 1u;
    for (t = 0u; t <= 600000u; t += 1000u) {
        CHECK(!od_adv_watch_step(&w, &s, t));
    }
    /* disconnect, RETRY for a few passes, then back up -- repeatedly */
    s.connection_count = 0u;
    for (t = 600000u; t < 600000u + 10u * OD_ADV_WATCH_RESET_MS; t += 5000u) {
        s.active = ((t / 5000u) % 2u) == 0u;
        CHECK(!od_adv_watch_step(&w, &s, t));
    }
    /* intent withdrawn (claim/boot wait) or stack not ready: not down */
    s.active = false;
    s.desired = false;
    CHECK(!od_adv_watch_step(&w, &s, 0u));
    CHECK(!od_adv_watch_step(&w, &s, OD_ADV_WATCH_RESET_MS * 2u));
    s.desired = true;
    s.stack_ready = false;
    CHECK(!od_adv_watch_step(&w, &s, OD_ADV_WATCH_RESET_MS * 3u));
}

static void test_wanted_but_never_running_resets(void)
{
    struct od_adv_watch w = {0};
    struct od_adv_control s = running();

    s.active = false;   /* wanted, idle, nobody connected, start keeps failing as RETRY */
    CHECK(!od_adv_watch_step(&w, &s, 0xFFFFF000u));                         /* across the wrap */
    CHECK(!od_adv_watch_step(&w, &s, 0xFFFFF000u + OD_ADV_WATCH_RESET_MS - 1u));
    CHECK(od_adv_watch_step(&w, &s, 0xFFFFF000u + OD_ADV_WATCH_RESET_MS));
}

int main(void)
{
    test_latched_fault_resets_after_bound();
    test_fault_counts_even_with_connection();
    test_connection_and_transients_do_not_reset();
    test_wanted_but_never_running_resets();
    if (failures != 0) {
        fprintf(stderr, "nordic_adv_watch: %d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("nordic_adv_watch: all passed\n");
    return EXIT_SUCCESS;
}
