/* action: od_action over a stored 0xB2 table -- every trigger, the temperature edges, the
 * validator vs the runner, and the uptime wrap. Effects are recorded through the
 * od_action_app seam; the table comes from the fake medium via od_cfg_index, as on target.
 */
#include "od_check.h"
#include "drive_fixture.h"

#include "od_action.h"
#include "od_action_app.h"
#include "od_cfg_index.h"

#include <limits.h>

/* ------------------------------------------------------------------------- effect seam -- */

static char     g_fx[2048];
static size_t   g_fx_n;
static unsigned g_led, g_slot, g_buzz;
static int8_t   g_temp = 20;

static void note(const char *s)
{
    const size_t n = strlen(s);
    if (g_fx_n + n + 2u < sizeof g_fx) {
        memcpy(g_fx + g_fx_n, s, n);
        g_fx_n += n;
        g_fx[g_fx_n++] = ' ';
        g_fx[g_fx_n] = '\0';
    }
}

void od_action_app_led(uint8_t colour, uint8_t brightness)
{
    char b[24];
    snprintf(b, sizeof b, "L%02x/%u", colour, (unsigned)brightness);
    note(b);
    g_led++;
}
void od_action_app_led_stop(void) { note("S"); }
void od_action_app_display_slot(uint8_t slot)
{
    char b[16];
    snprintf(b, sizeof b, "D%u", (unsigned)slot);
    note(b);
    g_slot++;
}
void od_action_app_buzzer(uint8_t melody)
{
    char b[16];
    snprintf(b, sizeof b, "B%u", (unsigned)melody);
    note(b);
    g_buzz++;
}
bool od_action_app_has_buzzer(void) { return true; }
int8_t od_action_app_temperature_c(void) { return g_temp; }

static void fx_reset(void)
{
    g_fx_n = 0u;
    g_fx[0] = '\0';
    g_led = g_slot = g_buzz = 0u;
}

/* ------------------------------------------------------------------------ table builder -- */

typedef struct {
    uint8_t b[512];
    uint32_t n;
} tbl_t;

static void ent(tbl_t *t, uint8_t trig, uint16_t arg, bool enabled, uint8_t steps,
                const uint8_t (*st)[4])
{
    uint8_t *e = &t->b[t->n];
    e[0] = (uint8_t)(OD_ACTION_ENTRY_HDR + steps * OD_ACTION_STEP_LEN);
    e[1] = trig;
    e[2] = (uint8_t)arg;
    e[3] = (uint8_t)(arg >> 8);
    e[4] = enabled ? 1u : 0u;
    e[5] = steps;
    for (uint8_t i = 0; i < steps; i++) {
        memcpy(&e[6u + 4u * i], st[i], 4u);
    }
    t->n += e[0];
}

static void store(const tbl_t *t)
{
    fx_cfg_t c;
    const struct DisplayConfig d = fx_display(0u, 16u, 4u, 0u);

    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, t->b, t->n);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    (void)od_cfg_index_build();
    od_action_reload();
    fx_reset();
}

/* Service until idle (bounded); returns the clock reached. */
static uint32_t run(uint32_t now, unsigned max_ticks)
{
    for (unsigned i = 0; i < max_ticks; i++) {
        const uint32_t next = od_action_service(now);
        if (next == OD_ACTION_IDLE) {
            break;
        }
        now += next > 3000u ? 3000u : next;
    }
    return now;
}

static const uint8_t ST_LED[1][4] = { { OD_EFF_LED, 0x21, 8, 0 } };
static const uint8_t ST_SLOT[1][4] = { { OD_EFF_DISPLAY_SLOT, 3, 0, 0 } };

/* -------------------------------------------------------------------------------- tests -- */

static void test_each_trigger(void)
{
    tbl_t t = { .n = 0u };
    static const uint8_t st_boot[2][4] = { { OD_EFF_LED, 0x11, 4, 10 }, { OD_EFF_LED_STOP, 0, 0, 0 } };
    static const uint8_t st_nfc[1][4] = { { OD_EFF_BUZZER, 2, 0, 0 } };
    static const uint8_t st_host[1][4] = { { OD_EFF_DISPLAY_SLOT, 7, 0, 0 } };
    static const uint8_t st_long[1][4] = { { OD_EFF_DISPLAY_SLOT, 9, 0, 0 } };

    ent(&t, OD_TRIG_BOOT, 0u, true, 2u, st_boot);
    ent(&t, OD_TRIG_BUTTON_PRESS, 0u, true, 1u, ST_LED);
    ent(&t, OD_TRIG_NFC_FIELD, 0u, true, 1u, st_nfc);
    ent(&t, OD_TRIG_HOST, 0x1234u, true, 1u, st_host);
    ent(&t, OD_TRIG_BUTTON_LONG, 200u, true, 1u, st_long);
    store(&t);

    CASE("reload counts five entries; BUTTON_LONG is inactive (no target emits it)");
    CHECK(od_action_stats()->entries == 5u && od_action_stats()->active == 4u);
    CHECK(od_action_stats()->rejected == 0u && od_action_stats()->inactive == 1u);

    CASE("BOOT: both steps, the delay honoured between them");
    od_action_trigger(OD_TRIG_BOOT, 0u);
    CHECK(od_action_service(1000u) == 100u);     /* LED, then wait 10 cs */
    CHECK(strcmp(g_fx, "L11/4 ") == 0);
    CHECK(od_action_service(1050u) == 50u);      /* not yet */
    CHECK(od_action_service(1100u) == OD_ACTION_IDLE);
    CHECK(strcmp(g_fx, "L11/4 S ") == 0);
    CHECK(od_action_stats()->last_trigger == OD_TRIG_BOOT && od_action_stats()->fire_count == 1u);

    CASE("BUTTON_PRESS arg 0 on the entry matches any pin");
    fx_reset();
    od_action_trigger(OD_TRIG_BUTTON_PRESS, 1u << 2);
    (void)run(2000u, 10u);
    CHECK(g_led == 1u);

    CASE("NFC_FIELD");
    fx_reset();
    od_action_trigger(OD_TRIG_NFC_FIELD, 0u);
    (void)run(3000u, 10u);
    CHECK(strcmp(g_fx, "B2 ") == 0);

    CASE("HOST: only the matching id");
    fx_reset();
    od_action_trigger(OD_TRIG_HOST, 0x1235u);
    (void)run(4000u, 10u);
    CHECK(g_slot == 0u);
    od_action_trigger(OD_TRIG_HOST, 0x1234u);
    (void)run(5000u, 10u);
    CHECK(strcmp(g_fx, "D7 ") == 0);

    CASE("BUTTON_LONG: arg is a hold time, matched by equality");
    fx_reset();
    od_action_trigger(OD_TRIG_BUTTON_LONG, 250u);   /* held longer than the entry asks */
    (void)run(6000u, 10u);
    /* No target fires BUTTON_LONG yet, so this is the contract question, not a live bug:
     * a hold time compared with == only fires for a press of exactly 2.00 s. Pinned. */
    CHECK(g_slot == 0u);

    CASE("a trigger during a running sequence is dropped, not queued");
    fx_reset();
    od_action_trigger(OD_TRIG_BOOT, 0u);
    CHECK(od_action_service(7000u) == 100u);
    od_action_trigger(OD_TRIG_NFC_FIELD, 0u);
    CHECK(od_action_service(7010u) == 90u);
    (void)run(7100u, 10u);
    (void)run(9000u, 10u);
    CHECK(g_buzz == 0u);

    CASE("trigger NONE and an unknown trigger do nothing");
    fx_reset();
    od_action_trigger(OD_TRIG_NONE, 0u);
    od_action_trigger(0x55u, 0u);
    (void)run(10000u, 10u);
    CHECK(g_fx_n == 0u);
}

static void test_button_mask(void)
{
    tbl_t t = { .n = 0u };

    CASE("BUTTON-MASK: entry arg is a BinaryInputs pin bitmask; a press on pin 1 matches mask 0x03");
    ent(&t, OD_TRIG_BUTTON_PRESS, 0x0003u, true, 1u, ST_LED);
    store(&t);
    /* opendisplay_ble.c fires (1u << button_id); the entry arg is a mask. */
    od_action_trigger(OD_TRIG_BUTTON_PRESS, 1u << 1);
    (void)run(1000u, 10u);
    CHECK(g_led == 1u);

    CASE("the exact mask value still matches");
    od_action_trigger(OD_TRIG_BUTTON_PRESS, 0x0003u);
    (void)run(2000u, 10u);
    CHECK(g_led == 2u);

    CASE("a pin outside the mask does not match");
    od_action_trigger(OD_TRIG_BUTTON_PRESS, 1u << 2);
    (void)run(3000u, 10u);
    CHECK(g_led == 2u);
}

static void test_enable_live(void)
{
    tbl_t t = { .n = 0u };
    static const uint8_t st[3][4] = { { OD_EFF_LED, 1, 1, 10 }, { OD_EFF_LED, 2, 2, 10 },
                                      { OD_EFF_LED, 3, 3, 0 } };

    CASE("a disabled entry never fires");
    ent(&t, OD_TRIG_HOST, 0u, false, 1u, ST_LED);
    store(&t);
    od_action_trigger(OD_TRIG_HOST, 0u);
    (void)run(1000u, 10u);
    CHECK(g_led == 0u);

    CASE("clearing the enable bit mid-sequence stops it at the next step");
    t.n = 0u;
    ent(&t, OD_TRIG_HOST, 0u, true, 3u, st);
    store(&t);
    od_action_trigger(OD_TRIG_HOST, 0u);
    CHECK(od_action_service(1000u) == 100u);
    t.b[4] = 0u;
    {
        fx_cfg_t c;
        const struct DisplayConfig d = fx_display(0u, 16u, 4u, 0u);
        fx_cfg_begin(&c, 1u);
        fx_cfg_legacy_head(&c, &d);
        fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, t.b, t.n);
        fx_cfg_finish(&c);
        fx_cfg_store(&c);
        (void)od_cfg_index_build();       /* config write, no reload */
    }
    CHECK(od_action_service(1100u) == OD_ACTION_IDLE);
    CHECK(g_led == 1u);
}

static void test_validator_vs_runner(void)
{
    tbl_t t = { .n = 0u };
    static uint8_t st9[9][4];

    CASE("ACT-REJECTED-RUNS: a 9-step entry is rejected at reload but still runs on trigger");
    for (unsigned i = 0; i < 9u; i++) {
        st9[i][0] = OD_EFF_LED;
        st9[i][1] = (uint8_t)i;
        st9[i][2] = 1u;
        st9[i][3] = 255u;                 /* 2.55 s each: past the 8-step bound by inspection */
    }
    ent(&t, OD_TRIG_HOST, 0u, true, 9u, (const uint8_t (*)[4])st9);
    store(&t);
    CHECK(od_action_stats()->rejected == 1u && od_action_stats()->active == 0u);
    od_action_trigger(OD_TRIG_HOST, 0u);
    (void)run(1000u, 40u);
    /* find_entry() re-checks only len >= 6 and the packet bound, not step_count against
     * entry_len or OD_ACTION_MAX_STEPS -- the bound od_action.h says makes a table judgeable
     * without running it. */
    KNOWN_BUG("ACT-REJECTED-RUNS", g_led == 0u);

    CASE("ACT-REJECTED-RUNS: step_count larger than entry_len runs into the next entry's bytes");
    t.n = 0u;
    ent(&t, OD_TRIG_HOST, 0u, true, 1u, ST_LED);
    t.b[5] = 2u;                          /* claims two steps, holds one */
    ent(&t, OD_TRIG_NFC_FIELD, 0x0403u, true, 1u, ST_SLOT);
    store(&t);
    CHECK(od_action_stats()->rejected == 1u);
    od_action_trigger(OD_TRIG_HOST, 0u);
    (void)run(1000u, 10u);
    /* Step 2 is the next entry's header [0a 04 03 04] read as effect 0x0a (unknown: delay
     * only). Harmless here only because 0x0a is not an effect id; an entry_len of 3 would
     * read as DISPLAY_SLOT. */
    KNOWN_BUG("ACT-REJECTED-RUNS", g_led == 0u);

    CASE("ACT-RELOAD-STOPS: one malformed entry hides every entry after it from the stats");
    /* od_action.h: "a fault found here drops one entry, never the table". reload() breaks
     * on the first fault; find_entry() still runs the later entries, so the device acts
     * on entries its own stats say do not exist. */
    CHECK(od_action_stats()->entries == 0u);
    fx_reset();
    od_action_trigger(OD_TRIG_NFC_FIELD, 0x0403u);
    (void)run(5000u, 10u);
    CHECK(g_slot == 1u);
    KNOWN_BUG("ACT-RELOAD-STOPS", od_action_stats()->entries == 1u);

    CASE("an unknown effect keeps the entry, inactive");
    t.n = 0u;
    {
        static const uint8_t st[1][4] = { { 0x40, 0, 0, 0 } };
        ent(&t, OD_TRIG_HOST, 0u, true, 1u, st);
    }
    store(&t);
    CHECK(od_action_stats()->entries == 1u && od_action_stats()->inactive == 1u);
    CHECK(od_action_stats()->rejected == 0u);

    CASE("an all-zero-delay entry yields between steps");
    t.n = 0u;
    {
        static const uint8_t st[3][4] = { { OD_EFF_LED, 1, 1, 0 }, { OD_EFF_LED, 2, 1, 0 },
                                          { OD_EFF_LED, 3, 1, 0 } };
        ent(&t, OD_TRIG_HOST, 0u, true, 3u, st);
    }
    store(&t);
    od_action_trigger(OD_TRIG_HOST, 0u);
    CHECK(od_action_service(100u) == 0u && g_led == 1u);
    CHECK(od_action_service(100u) == 0u && g_led == 2u);
    CHECK(od_action_service(100u) == OD_ACTION_IDLE && g_led == 3u);
}

/* Poll once at `now` with temperature `c`; returns how many sequences started. */
static unsigned poll_at(uint32_t now, int8_t c)
{
    const uint8_t before = od_action_stats()->fire_count;
    g_temp = c;
    (void)run(now, 4u);
    return (uint8_t)(od_action_stats()->fire_count - before);
}

static void test_temperature(void)
{
    tbl_t t = { .n = 0u };
    unsigned fires = 0u;
    uint32_t now = 10000u;

    CASE("TEMP_ENTER: boot inside the window is a baseline, not an edge");
    ent(&t, OD_TRIG_TEMP_ENTER, (uint16_t)((uint8_t)20 | ((uint16_t)(uint8_t)25 << 8)), true,
        1u, ST_LED);
    store(&t);
    CHECK(od_action_stats()->active == 1u);
    CHECK(poll_at(now, 22) == 0u);
    now += 2000u;
    CHECK(poll_at(now, 26) == 0u);       /* inside the 3 C exit band: still in */
    now += 2000u;
    CHECK(poll_at(now, 29) == 0u);       /* out: past hi + 3 */
    now += 2000u;
    CHECK(poll_at(now, 25) == 1u);       /* in: the edge, inclusive at hi */
    now += 2000u;
    CHECK(poll_at(now, 24) == 0u);       /* still in */

    CASE("hysteresis: +-1 C of sensor noise at the edge does not re-fire");
    {
        static const int8_t noise[] = { 26, 25, 26, 25, 26, 25, 26, 25 };
        for (unsigned i = 0; i < sizeof noise; i++) {
            now += 2000u;
            fires += poll_at(now, noise[i]);
        }
    }
    /* Exit needs t > hi + OD_ACTION_TEMP_HYST_C (3 C), so the dither at 25/26 never leaves. */
    CHECK(fires == 0u);

    CASE("polls closer than 2 s apart do not sample");
    now += 2000u;
    (void)poll_at(now, 29);
    CHECK(poll_at(now + 500u, 25) == 0u);
    CHECK(poll_at(now + 2000u, 25) == 1u);
    now += 2000u;

    CASE("negative window: arg bytes are signed");
    t.n = 0u;
    ent(&t, OD_TRIG_TEMP_ENTER, (uint16_t)((uint8_t)(int8_t)-10 | ((uint16_t)(uint8_t)(int8_t)-1 << 8)),
        true, 1u, ST_LED);
    store(&t);
    CHECK(poll_at(now, 5) == 0u);
    now += 2000u;
    CHECK(poll_at(now, -5) == 1u);
    now += 2000u;

    CASE("TEMP-SHARED-EDGE: two windows, one ENTER when moving into the second");
    t.n = 0u;
    ent(&t, OD_TRIG_TEMP_ENTER, (uint16_t)(0u | (10u << 8)), true, 1u, ST_LED);
    ent(&t, OD_TRIG_TEMP_ENTER, (uint16_t)(20u | (30u << 8)), true, 1u, ST_SLOT);
    store(&t);
    CHECK(poll_at(now, 15) == 0u);       /* baseline, outside both */
    fires = 0u;
    for (unsigned i = 0; i < 5u; i++) {  /* move into the second window and stay there */
        now += 2000u;
        fires += poll_at(now, 25);
    }
    /* TEMP-SHARED-EDGE: each temperature entry keeps its own edge state, so a second window
     * fires once on entry, not on every 2 s poll as a level. */
    CHECK(fires == 1u);
    CHECK(g_slot == 1u);

    CASE("no sensor: temperature entries report inactive");
    g_temp = INT8_MIN;
    store(&t);
    CHECK(od_action_stats()->inactive == 2u && od_action_stats()->active == 0u);
    g_temp = 20;
}

static void test_wrap(void)
{
    tbl_t t = { .n = 0u };
    static const uint8_t st[2][4] = { { OD_EFF_LED, 1, 1, 100 }, { OD_EFF_LED, 2, 1, 0 } };

    CASE("uptime wrap at 2^32 ms: a 1 s delay spanning the wrap");
    ent(&t, OD_TRIG_HOST, 0u, true, 2u, st);
    store(&t);
    od_action_trigger(OD_TRIG_HOST, 0u);
    const uint32_t t0 = 0xFFFFFF00u;
    const uint32_t r0 = od_action_service(t0);
    /* next_ms wrapped to 0x2E8; `next > now` is false, so the return reads 0 ("call me now")
     * instead of 1000. The following call recovers via the signed compare, so the cost is one
     * spurious wake per wrap (every 49.7 days on BG22, whose uptime is the 64-bit sleeptimer
     * cast down). Pinned. */
    CHECK(r0 == 0u);
    CHECK(od_action_service(t0 + 1u) == 999u);
    CHECK(g_led == 1u);
    CHECK(od_action_service(t0 + 1000u) == OD_ACTION_IDLE);
    CHECK(g_led == 2u);

    CASE("temperature polling keeps running across the wrap");
    t.n = 0u;
    ent(&t, OD_TRIG_TEMP_ENTER, (uint16_t)(20u | (25u << 8)), true, 1u, ST_LED);
    store(&t);
    CHECK(poll_at(0xFFFFF000u, 30) == 0u);
    CHECK(poll_at(0xFFFFF000u + 2000u, 30) == 0u);
    CHECK(poll_at(0xFFFFF000u + 4000u, 22) == 1u);   /* now = 0x0000_0BA0 */
}

int main(void)
{
    test_each_trigger();
    test_button_mask();
    test_enable_live();
    test_validator_vs_runner();
    test_temperature();
    test_wrap();
    printf("action: %u known bugs\n", fx_xfail_count);
    return OD_CHECK_REPORT_NONEMPTY("action", 50);
}
