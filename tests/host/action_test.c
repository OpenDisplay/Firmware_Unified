/* action_test.c -- od_action over a fake config index and fake effects.
 *
 * What is pinned: temperature edges with hysteresis (one ENTER and one EXIT per real crossing,
 * however much the reading dithers at the edge), per-entry edge state for entries with
 * different windows, the BUTTON_PRESS pin-mask match, and the duplicate-0xB2 count.
 */
#include "od_action.h"
#include "od_action_app.h"
#include "od_cfg_index.h"
#include "od_config_tlv.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned g_checks, g_failures;
static const char *g_case = "(none)";
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; \
    printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #c); } } while (0)
#define CASE(n) (g_case = (n))

/* ------------------------------------------------------------------ fake od_cfg_index --- */
#define MAX_TABLES 3
static uint8_t g_body[MAX_TABLES][128];
static od_cfg_entry_t g_ent[MAX_TABLES];
static unsigned g_ntables;

const od_cfg_entry_t *od_cfg_find(uint8_t type, uint8_t nth)
{
    if (type != OD_PKT_ACTION_TABLE || nth >= g_ntables) {
        return NULL;
    }
    return &g_ent[nth];
}

uint32_t od_cfg_read(const od_cfg_entry_t *e, uint16_t off, uint8_t *dst, uint16_t len)
{
    const unsigned t = (unsigned)(e - g_ent);
    if ((uint32_t)off + len > e->len) {
        return 0u;
    }
    memcpy(dst, &g_body[t][off], len);
    return len;
}

/* ------------------------------------------------------------------------ fake effects --- */
static int8_t g_temp;
static unsigned g_led_calls;
static uint8_t g_last_colour;

void od_action_app_led(uint8_t colour, uint8_t brightness)
{
    (void)brightness;
    g_led_calls++;
    g_last_colour = colour;
}
void od_action_app_led_stop(void) { }
void od_action_app_display_slot(uint8_t slot) { (void)slot; }
void od_action_app_buzzer(uint8_t melody) { (void)melody; }
bool od_action_app_has_buzzer(void) { return false; }
int8_t od_action_app_temperature_c(void) { return g_temp; }

/* --------------------------------------------------------------------- table building --- */
static uint16_t g_len[MAX_TABLES];

static void table_reset(void)
{
    memset(g_body, 0, sizeof g_body);
    memset(g_ent, 0, sizeof g_ent);
    memset(g_len, 0, sizeof g_len);
    g_ntables = 1u;
}

/* One entry: trigger, arg, one LED step whose colour identifies the entry. */
static void add_entry(unsigned t, uint8_t trig, uint16_t arg, uint8_t colour)
{
    uint8_t *p = &g_body[t][g_len[t]];
    p[0] = OD_ACTION_ENTRY_HDR + OD_ACTION_STEP_LEN;
    p[1] = trig;
    p[2] = (uint8_t)arg;
    p[3] = (uint8_t)(arg >> 8);
    p[4] = 0x01u;                /* enabled */
    p[5] = 1u;
    p[6] = OD_EFF_LED;
    p[7] = colour;
    p[8] = 1u;
    p[9] = 0u;                   /* no delay: the sequence completes in the same pass */
    g_len[t] = (uint16_t)(g_len[t] + p[0]);
    g_ent[t].type = OD_PKT_ACTION_TABLE;
    g_ent[t].len = g_len[t];
}

static uint16_t window(int8_t lo, int8_t hi)
{
    return (uint16_t)((uint8_t)lo | ((uint16_t)(uint8_t)hi << 8));
}

static uint32_t g_now;

/* One poll period per sample, counting LED effects by colour. */
static void feed(const int8_t *samples, unsigned n, unsigned *enter, unsigned *exit_)
{
    for (unsigned i = 0u; i < n; i++) {
        g_temp = samples[i];
        g_led_calls = 0u;
        g_last_colour = 0u;
        (void)od_action_service(g_now);
        if (g_led_calls != 0u) {
            if (g_last_colour == 0xE1u) (*enter)++;
            if (g_last_colour == 0xE0u) (*exit_)++;
        }
        g_now += 2000u;
    }
}

static void test_temp_hysteresis(void)
{
    /* The OD58FFC4 ramp shape: a slow fall through the 32 C edge of a 32..48 window with
     * +/-2 C dither, then on below the band; then back up through it the same way. */
    static const int8_t fall[] = { 39, 40, 38, 39, 35, 34, 36, 35, 33, 32, 33, 31, 33, 30, 32,
                                   31, 29, 30, 28, 30, 27, 29, 26, 25, 24 };
    static const int8_t rise[] = { 25, 27, 26, 29, 28, 30, 29, 31, 30, 32, 31, 33, 32, 34, 33,
                                   36, 38 };
    unsigned enter = 0u, exit_ = 0u;

    table_reset();
    add_entry(0, OD_TRIG_TEMP_ENTER, window(32, 48), 0xE1u);
    add_entry(0, OD_TRIG_TEMP_EXIT, window(32, 48), 0xE0u);
    od_action_reload();
    g_now = 1000u;

    CASE("first sample is a baseline, never an edge");
    feed(fall, 1u, &enter, &exit_);
    CHECK(enter == 0u && exit_ == 0u);

    CASE("dithering across the lower edge, then out: exactly one EXIT, no ENTER");
    feed(&fall[1], (unsigned)(sizeof fall - 1u), &enter, &exit_);
    CHECK(exit_ == 1u);
    CHECK(enter == 0u);

    CASE("dithering back up across it: exactly one ENTER, no further EXIT");
    feed(rise, (unsigned)sizeof rise, &enter, &exit_);
    CHECK(enter == 1u);
    CHECK(exit_ == 1u);

    CASE("ENTER fires at the configured edge, not inside the band");
    table_reset();
    add_entry(0, OD_TRIG_TEMP_ENTER, window(32, 48), 0xE1u);
    od_action_reload();
    enter = exit_ = 0u;
    {
        static const int8_t s[] = { 20, 31, 32 };
        feed(s, 2u, &enter, &exit_);
        CHECK(enter == 0u);
        feed(&s[2], 1u, &enter, &exit_);
        CHECK(enter == 1u);
    }

    CASE("EXIT needs more than 3 C past the edge -- upper edge too");
    table_reset();
    add_entry(0, OD_TRIG_TEMP_EXIT, window(32, 48), 0xE0u);
    od_action_reload();
    enter = exit_ = 0u;
    {
        static const int8_t s[] = { 40, 49, 50, 51, 50, 51, 52 };
        feed(s, (unsigned)sizeof s, &enter, &exit_);
        CHECK(exit_ == 1u);
    }

    CASE("negative windows");
    table_reset();
    add_entry(0, OD_TRIG_TEMP_EXIT, window(-10, 0), 0xE0u);
    od_action_reload();
    enter = exit_ = 0u;
    {
        static const int8_t s[] = { -5, -11, -12, -13, -14 };
        feed(s, (unsigned)sizeof s, &enter, &exit_);
        CHECK(exit_ == 1u);
        CHECK(od_action_stats()->last_temp_c == -14);
    }

    CASE("no sensor: nothing fires and the entry is inactive");
    table_reset();
    add_entry(0, OD_TRIG_TEMP_ENTER, window(0, 10), 0xE1u);
    g_temp = INT8_MIN;
    od_action_reload();
    CHECK(od_action_stats()->inactive == 1u);
}

static void test_temp_per_entry(void)
{
    unsigned enter = 0u, exit_ = 0u;

    CASE("two entries with different windows keep separate edge state");
    table_reset();
    add_entry(0, OD_TRIG_TEMP_ENTER, window(30, 40), 0xE1u);   /* entered at 30 */
    add_entry(0, OD_TRIG_TEMP_EXIT, window(10, 20), 0xE0u);    /* left above 23 */
    g_temp = 15;
    od_action_reload();
    g_now = 1000u;
    {
        /* 15: inside B, outside A. Rising to 25 leaves B (one EXIT); 30 enters A (one ENTER).
         * A shared edge bit tracked only the first entry's window. */
        static const int8_t s[] = { 15, 25 };
        feed(s, 2u, &enter, &exit_);
        CHECK(exit_ == 1u && enter == 0u);
        static const int8_t t[] = { 30 };
        feed(t, 1u, &enter, &exit_);
        CHECK(enter == 1u && exit_ == 1u);
    }

    CASE("disabled entries do not fire but keep tracking");
    table_reset();
    add_entry(0, OD_TRIG_TEMP_ENTER, window(30, 40), 0xE1u);
    g_body[0][4] = 0u;
    od_action_reload();
    enter = exit_ = 0u;
    {
        static const int8_t s[] = { 20, 35 };
        feed(s, 2u, &enter, &exit_);
        CHECK(enter == 0u);
    }
}

static void test_button_mask(void)
{
    CASE("a BUTTON_PRESS mask matches any button in it");
    table_reset();
    add_entry(0, OD_TRIG_BUTTON_PRESS, 0x0006u, 0xB1u);   /* buttons 1 and 2 */
    g_temp = INT8_MIN;
    od_action_reload();
    g_now = 1000u;

    g_led_calls = 0u;
    od_action_trigger(OD_TRIG_BUTTON_PRESS, 1u << 2);
    (void)od_action_service(g_now += 10u);
    CHECK(g_led_calls == 1u && g_last_colour == 0xB1u);

    g_led_calls = 0u;
    od_action_trigger(OD_TRIG_BUTTON_PRESS, 1u << 1);
    (void)od_action_service(g_now += 10u);
    CHECK(g_led_calls == 1u);

    CASE("a button outside the mask does not");
    g_led_calls = 0u;
    od_action_trigger(OD_TRIG_BUTTON_PRESS, 1u << 0);
    (void)od_action_service(g_now += 10u);
    CHECK(g_led_calls == 0u);

    CASE("HOST still matches its id exactly, not as a mask");
    table_reset();
    add_entry(0, OD_TRIG_HOST, 0x0003u, 0xC3u);
    od_action_reload();
    g_led_calls = 0u;
    od_action_trigger(OD_TRIG_HOST, 0x0001u);
    (void)od_action_service(g_now += 10u);
    CHECK(g_led_calls == 0u);
    od_action_trigger(OD_TRIG_HOST, 0x0003u);
    (void)od_action_service(g_now += 10u);
    CHECK(g_led_calls == 1u);
}

static void test_duplicate_tables(void)
{
    CASE("no table: zero tables");
    table_reset();
    g_ntables = 0u;
    od_action_reload();
    CHECK(od_action_stats()->tables == 0u);

    CASE("one table");
    table_reset();
    add_entry(0, OD_TRIG_HOST, 1u, 0x01u);
    od_action_reload();
    CHECK(od_action_stats()->tables == 1u);

    CASE("three tables are counted; only the first one runs");
    add_entry(1, OD_TRIG_HOST, 1u, 0x02u);
    add_entry(2, OD_TRIG_HOST, 1u, 0x03u);
    g_ntables = 3u;
    od_action_reload();
    CHECK(od_action_stats()->tables == 3u);
    CHECK(od_action_stats()->entries == 1u);
    g_led_calls = 0u;
    od_action_trigger(OD_TRIG_HOST, 1u);
    (void)od_action_service(g_now += 10u);
    CHECK(g_led_calls == 1u && g_last_colour == 0x01u);
}

int main(void)
{
    test_temp_hysteresis();
    test_temp_per_entry();
    test_button_mask();
    test_duplicate_tables();
    printf("action: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
