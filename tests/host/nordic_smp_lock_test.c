/* nordic_smp_lock_test.c -- H3 (b): an ENTER_DFU unlock of SMP lasts one session. It ends when
 * the first link opened after the unlock closes, or when no link opens within the bound; the
 * dropped ENTER_DFU link never ends it, and an open session is never cut. */
#include "od_smp_lock.h"

#include <stdio.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* opens/closes as the BT callbacks count them; one link at a time */
static uint32_t opens, closes;

static void link_open(void)  { opens++; }
static void link_close(void) { closes++; }

static void reset_links(uint32_t start)
{
    opens = start;
    closes = start;
}

/* The normal flow: ENTER_DFU on link 1, firmware drops it, host reconnects, uploads, leaves. */
static void test_session_link_close_relocks(void)
{
    struct od_smp_lock l = {0};

    reset_links(0u);
    link_open();                                  /* authenticated link sends ENTER_DFU */
    od_smp_lock_unlock(&l, opens, 1000u);
    CHECK(!od_smp_lock_step(&l, opens, closes, 1100u));
    link_close();                                 /* dfu_work drops the ENTER_DFU link */
    CHECK(!od_smp_lock_step(&l, opens, closes, 1200u));
    link_open();                                  /* the session link */
    CHECK(!od_smp_lock_step(&l, opens, closes, 2000u));
    /* a long upload outlives the connect bound and is not cut */
    CHECK(!od_smp_lock_step(&l, opens, closes, 1000u + 10u * OD_SMP_UNLOCK_CONNECT_MS));
    link_close();
    CHECK(od_smp_lock_step(&l, opens, closes, 1000u + 10u * OD_SMP_UNLOCK_CONNECT_MS + 1u));
    CHECK(!l.unlocked);
    CHECK(!od_smp_lock_step(&l, opens, closes, 1000u + 10u * OD_SMP_UNLOCK_CONNECT_MS + 2u));
}

/* The loop may see all three edges in one pass: still one session, still ended by its close. */
static void test_edges_seen_late_in_one_pass(void)
{
    struct od_smp_lock l = {0};

    reset_links(7u);
    link_open();
    od_smp_lock_unlock(&l, opens, 0u);
    link_close();
    link_open();
    CHECK(!od_smp_lock_step(&l, opens, closes, 50u));
    link_close();
    CHECK(od_smp_lock_step(&l, opens, closes, 60u));
}

/* The ENTER_DFU link already closed when the unlock is recorded: the next open is the session. */
static void test_unlock_recorded_after_drop(void)
{
    struct od_smp_lock l = {0};

    reset_links(3u);
    link_open();
    link_close();
    od_smp_lock_unlock(&l, opens, 0u);
    CHECK(!od_smp_lock_step(&l, opens, closes, 10u));
    link_open();
    CHECK(!od_smp_lock_step(&l, opens, closes, 20u));
    link_close();
    CHECK(od_smp_lock_step(&l, opens, closes, 30u));
}

/* Nobody comes back: re-lock after the bound, not before. */
static void test_no_session_times_out(void)
{
    struct od_smp_lock l = {0};

    reset_links(0u);
    link_open();
    od_smp_lock_unlock(&l, opens, 5000u);
    link_close();
    CHECK(!od_smp_lock_step(&l, opens, closes, 5000u + OD_SMP_UNLOCK_CONNECT_MS - 1u));
    CHECK(od_smp_lock_step(&l, opens, closes, 5000u + OD_SMP_UNLOCK_CONNECT_MS));
}

/* Counters and uptime wrap without ending or extending the unlock. */
static void test_wrap(void)
{
    struct od_smp_lock l = {0};
    const uint32_t t0 = 0xFFFFFF00u;

    reset_links(0xFFFFFFFFu);
    link_open();                                  /* opens wraps to 0 */
    od_smp_lock_unlock(&l, opens, t0);
    link_close();
    CHECK(!od_smp_lock_step(&l, opens, closes, t0 + 0x200u));
    link_open();
    CHECK(!od_smp_lock_step(&l, opens, closes, t0 + OD_SMP_UNLOCK_CONNECT_MS * 2u));
    link_close();
    CHECK(od_smp_lock_step(&l, opens, closes, t0 + OD_SMP_UNLOCK_CONNECT_MS * 2u + 1u));

    reset_links(0u);
    link_open();
    od_smp_lock_unlock(&l, opens, t0);
    link_close();
    CHECK(!od_smp_lock_step(&l, opens, closes, t0 + OD_SMP_UNLOCK_CONNECT_MS - 1u));
    CHECK(od_smp_lock_step(&l, opens, closes, t0 + OD_SMP_UNLOCK_CONNECT_MS));
}

/* Never unlocked: never reports a re-lock. */
static void test_locked_stays_quiet(void)
{
    struct od_smp_lock l = {0};

    reset_links(0u);
    link_open();
    link_close();
    CHECK(!od_smp_lock_step(&l, opens, closes, OD_SMP_UNLOCK_CONNECT_MS * 3u));
}

int main(void)
{
    test_session_link_close_relocks();
    test_edges_seen_late_in_one_pass();
    test_unlock_recorded_after_drop();
    test_no_session_times_out();
    test_wrap();
    test_locked_stays_quiet();
    if (failures != 0) {
        fprintf(stderr, "nordic_smp_lock: %d failures\n", failures);
        return 1;
    }
    printf("nordic_smp_lock: ok\n");
    return 0;
}
