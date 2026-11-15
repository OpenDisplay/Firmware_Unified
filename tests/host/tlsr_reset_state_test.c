/* tlsr_reset_state_test.c -- the Telink reset-cause byte (targets/telink-tlsr/src/
 * od_tlsr_reset_state.h), walked through the boots a tag sees: power-on, planned reboots,
 * unplanned resets outside and inside a boot-screen render. The property behind L1: only a
 * reset that cut a render stops the boot screen, and that holds until power is removed. */
#include "od_tlsr_reset_state.h"

#include <stdio.h>

static unsigned g_checks, g_failures;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            ++g_failures;                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                  \
        }                                                                       \
    } while (0)

static uint8_t g_reg;
static struct od_tlsr_rs g_s;

/* A boot as od_tlsr_hal_boot() does it. */
static void boot(void)
{
    od_tlsr_rs_decode(g_reg, &g_s);
    g_reg = od_tlsr_rs_encode(&g_s, false, false);
}

static void render_begin(void) { g_reg = od_tlsr_rs_encode(&g_s, true, false); }
static void render_end(void) { g_reg = od_tlsr_rs_encode(&g_s, false, false); }
static void planned_reboot(void) { g_reg = od_tlsr_rs_encode(&g_s, false, true); }

static void power_on_values(void)
{
    static const uint8_t junk[] = { 0x00, 0xFF, 0x0F, 0x90, 0xD3, 0x5A };
    unsigned i;

    for (i = 0u; i < sizeof junk; ++i) {
        g_reg = junk[i];
        boot();
        CHECK(g_s.cause == OD_TLSR_RS_POWER_ON && g_s.unplanned == 0u && !g_s.render_died);
        CHECK(g_reg == OD_TLSR_RS_IDLE);
    }
}

static void unrelated_reset_keeps_boot_screen(void)
{
    g_reg = 0u;
    boot();
    render_begin();
    render_end();
    boot(); /* the reset pin, or a watchdog outside a render */
    CHECK(g_s.cause == OD_TLSR_RS_UNPLANNED && g_s.unplanned == 1u);
    CHECK(!g_s.render_died);
    planned_reboot();
    boot();
    CHECK(g_s.cause == OD_TLSR_RS_PLANNED_BOOT && g_s.unplanned == 1u && !g_s.render_died);
}

static void reset_in_render_is_sticky(void)
{
    unsigned i;

    g_reg = 0u;
    boot();
    render_begin();
    boot(); /* the watchdog fired inside the render */
    CHECK(g_s.cause == OD_TLSR_RS_UNPLANNED && g_s.unplanned == 1u && g_s.render_died);
    CHECK((g_reg & OD_TLSR_RS_MARK_MASK) == OD_TLSR_RS_DIED);

    /* a later render that completes, planned reboots and more resets keep it */
    render_begin();
    render_end();
    planned_reboot();
    boot();
    CHECK(g_s.render_died && g_s.cause == OD_TLSR_RS_PLANNED_BOOT);
    for (i = 0u; i < 10u; ++i) {
        boot();
    }
    CHECK(g_s.render_died && g_s.unplanned == OD_TLSR_RS_COUNT);

    g_reg = 0x00u; /* power removed */
    boot();
    CHECK(!g_s.render_died && g_s.cause == OD_TLSR_RS_POWER_ON);
}

static void planned_reboot_mid_render(void)
{
    /* not a death: the reboot was deliberate */
    g_reg = 0u;
    boot();
    g_reg = od_tlsr_rs_encode(&g_s, true, true);
    boot();
    CHECK(g_s.cause == OD_TLSR_RS_PLANNED_BOOT && !g_s.render_died && g_s.unplanned == 0u);
}

int main(void)
{
    power_on_values();
    unrelated_reset_keeps_boot_screen();
    reset_in_render_is_sticky();
    planned_reboot_mid_render();
    printf("tlsr_reset_state: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
