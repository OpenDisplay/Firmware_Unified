#include "od_touch_gt911.h"

#include "od_adv_app.h"
#include "od_hal_i2c.h"
#include "od_log.h"
#include "od_touch_app.h"
#include "opendisplay_structs.h" /* OD_STATIC_ASSERT */

#include <string.h>

/* CAPABILITY-OFF IS A REAL ARM, NOT A STUB. A target whose profile declines touch has no
 * touch_controllers[] in struct od_config at all, so every walk below would fail to compile. The
 * entry points still exist and answer truthfully -- idle, no address, not an interrupt pin -- so a
 * caller needs no #if of its own and the linker carries no state, no seam reference and no
 * register table. Same shape as od_nfc.c at OD_CAP_NFC=0. */
#if OD_CONFIG_WITH_TOUCH

/* --------------------------------------------------------------------- GT911 register map
 *
 * GOODIX "GT911 Programming Guide" Rev.10, 3.3. Register ADDRESSES are big-endian on the wire
 * (2.1); register CONTENTS are little-endian. That inversion inside one protocol is the most
 * common way this part is got wrong, so both directions are written out where they are used.
 */
#define GT911_REG_PID     0x8140u   /* 4 ASCII bytes, then fw version, then x/y resolution */
#define GT911_REG_STATUS  0x814Eu   /* bit7 buffer ready, bits3..0 contact count */
#define GT911_REG_POINT1  0x814Fu   /* 8-byte stride: id, x lo/hi, y lo/hi, size lo/hi, rsvd */

#define GT911_STATUS_BUFFER_READY 0x80u

/* Not a documented bound. 0x814E's count field is 4 bits wide and the guide never limits it; the
 * 1..5 range is the CONFIGURATION register 0x804C. Rev.10's own map enumerates six point blocks
 * through 0x817F, and HotKnot proximity adds a phantom contact with track id 32 aliased onto
 * point 1. Neither applies to this fleet, so the discard stands -- but a 6-contact panel would
 * need this revisited rather than a bug hunt. */
#define GT911_MAX_CONTACTS 5u

#define GT911_I2C_RETRIES        3u
#define GT911_I2C_RETRY_DELAY_US 500u

/* Reset timings. These are the guide's documented minima with margin, NOT arbitrary numbers:
 * 4.1's T1 > 100 us (RST low), T2 > 100 us (INT at the address-select level before RST rises),
 * T3 > 5 ms (INT held after it) and T4 > 50 ms (INT driven low before the host floats it). The
 * donor performs the T4 step that ESP-BSP's widely deployed driver omits. Do not tidy them. */
#define GT911_RST_LOW_MS        11u
#define GT911_INT_SETUP_US     110u
#define GT911_INT_HOLD_MS        6u
#define GT911_INT_LOW_MS        51u
#define GT911_POST_RESET_SETTLE_MS 200u
#define GT911_PRE_RESET_DELAY_MS   300u

#define GT911_ADDR_5D 0x5Du
#define GT911_ADDR_14 0x14u

#define TOUCH_DEFAULT_INTERVAL_MS      100u
#define TOUCH_I2C_FAIL_BACKOFF_MS      100u
#define TOUCH_I2C_FAIL_DISABLE_THRESHOLD 5u

#define OD_MSD_DYNAMIC_LEN 11u
#define TOUCH_MSD_BLOCK    5u
#define TOUCH_MSD_MAX_START (OD_MSD_DYNAMIC_LEN - TOUCH_MSD_BLOCK)   /* 6, and the header's @max */

struct touch_runtime {
    uint8_t  addr7;           /* 0 until resolved */
    uint8_t  bus_id;
    uint8_t  ok;
    uint8_t  reg_high_first;  /* 1 = documented order; 0 = the undocumented one, kept as fallback */
    uint8_t  int_attached;
    /* The pin the ISR is actually on; 0xFF means none.
     *
     * NOT 0, though memset would make that free: 0 is a LEGAL interrupt pin -- ESP32 GPIO0, and
     * Nordic P0.00, which the compact encoding writes as 0x00 -- and every int_pin test in this
     * file is `!= 0xFF`, not valid_pin(). With 0 as the sentinel a controller on pin 0 would be
     * invisible to the stale-pin detach, to disable, and to init's detach-by-record, reinstating
     * the orphaned-ISR bug this field exists to close. So every clear goes through
     * touch_rt_clear(). `int_attached` alone cannot survive a
     * config reload that MOVES the interrupt pin: the new pin gets attached and the old one keeps
     * its ISR for ever -- spurious service on ESP32, and on Nordic a permanently consumed callback
     * slot out of eight, so eventually no controller can attach at all. */
    uint8_t  int_pin_attached;
    uint8_t  disabled;
    uint8_t  latched;         /* a contact has been seen, so a release is reportable */
    uint8_t  fail_streak;
    uint8_t  last_count;
    uint8_t  last_id;
    uint16_t last_x;
    uint16_t last_y;
    uint32_t last_poll_ms;
    uint32_t last_fail_ms;
};

/* int_pin_attached's "no pin" is 0xFF, so the static needs it spelled out: a zeroed slot names
 * pin 0, which is legal, and init's detach-by-record would clear whatever ISR is on it before any
 * controller has been brought up.
 *
 * NOTHING IN THE LANGUAGE CHECKS THIS LIST. The assert below pins the ARRAY at four; a short
 * initializer list is zero-filled in silence, which is the bug itself. The cold-start case in
 * tests/host/touch_gt911_test.c is the check -- it is first in main() because setup() spends the
 * process's only cold start. */
static struct touch_runtime s_rt[OD_TOUCH_MAX_CONTROLLERS] = {
    { .int_pin_attached = 0xFFu },
    { .int_pin_attached = 0xFFu },
    { .int_pin_attached = 0xFFu },
    { .int_pin_attached = 0xFFu },
};
static uint8_t s_suspend;

/* THE IRQ MASK IS A uint8_t AND EVERY ADAPTER HARDCODES FOUR ISRs. Raising OD_CONFIG_MAX_TOUCH
 * without touching those would silently NULL-fill the ISR tables, and past eight would silently
 * truncate the mask. CLAUDE.md decision 1 asks for exactly this kind of assert. */
OD_STATIC_ASSERT(OD_TOUCH_MAX_CONTROLLERS <= 8u,
               "the IRQ mask is 8 bits wide; widen od_touch_gt911_service() first");
OD_STATIC_ASSERT(OD_TOUCH_MAX_CONTROLLERS == 4u,
               "both adapters define exactly four ISRs; add them before raising this");

/* --------------------------------------------------------------------- config helpers */

static const struct TouchController *touch_cfg(const struct od_config *cfg, uint8_t i)
{
    if (cfg == NULL || i >= cfg->touch_controller_count || i >= OD_TOUCH_MAX_CONTROLLERS) {
        return NULL;
    }
    if (cfg->touch_controllers[i].touch_ic_type != OD_TOUCH_IC_GT911) {
        return NULL;
    }
    return &cfg->touch_controllers[i];
}

/* A CONFIGURED IC THIS FIRMWARE DOES NOT IMPLEMENT IS NOT THE SAME AS NO TOUCH, and both donors
 * say so once at init. Folding the two into a silent skip leaves a board provisioned with a future
 * touch_ic_type booting with touch dead and nothing in the log to explain it -- on the one target
 * anyone can put on a bench. */
static void touch_log_unsupported(const struct od_config *cfg, uint8_t i)
{
    const uint16_t ic = cfg->touch_controllers[i].touch_ic_type;

    if (ic != OD_TOUCH_IC_NONE && ic != OD_TOUCH_IC_GT911) {
        od_log_warn("Touch[%u]: skipped (only GT911=1 implemented, got %u)", i, (unsigned)ic);
    }
}

/* THE ONLY WAY A RUNTIME IS CLEARED. memset alone would leave int_pin_attached at 0, which is a
 * legal pin -- see the field's comment. */
static void touch_rt_clear(struct touch_runtime *rt)
{
    memset(rt, 0, sizeof *rt);
    rt->int_pin_attached = 0xFFu;
}

static bool valid_pin(uint8_t pin)
{
    return pin != 0u && pin != 0xFFu;
}

/* A DECLARED BUS IS REQUIRED. `0xFF` is the absent sentinel and is refused rather than resolved
 * to bus 0; so is a declared bus_id on a config carrying no data_buses at all. The shared HAL is
 * keyed by DataBus.instance_number, so a bus that appears in no record has no identity this
 * driver could name -- and "whichever bus was selected last" is how an address collision returns
 * plausible-but-wrong contacts instead of nothing. DIVERGENCE_MATRIX 13 and 13.1. */
static bool touch_bus_ok(const struct od_config *cfg, const struct TouchController *t)
{
    const struct DataBus *bus;

    if (t->bus_id == 0xFFu) {
        return false;
    }
    bus = od_config_data_bus(cfg, t->bus_id);
    return bus != NULL && bus->bus_type == 0x01u && bus->pin_1 != 0xFFu && bus->pin_2 != 0xFFu;
}

/* --------------------------------------------------------------------- I2C */

static bool gt911_read_once(uint8_t bus_id, uint8_t addr7, uint16_t reg, uint8_t *buf, uint8_t len,
                            bool high_first, bool repeated_start)
{
    uint8_t sel[2];

    if (high_first) {
        sel[0] = (uint8_t)(reg >> 8);
        sel[1] = (uint8_t)(reg & 0xFFu);
    } else {
        sel[0] = (uint8_t)(reg & 0xFFu);
        sel[1] = (uint8_t)(reg >> 8);
    }
    /* Rev.10 2.2 permits BOTH: "The Stop condition ... is optional. However, the repeated Start
     * condition has to be sent." Keeping both is donor behaviour from ESP32 -- Firmware_NRF54
     * only ever issued the repeated START -- and it is cheap. The survey found no evidence for
     * the "some clones need one form" claim the first draft asserted, so this is retained as
     * inherited behaviour, not as a fact about clones. */
    if (repeated_start) {
        return od_hal_i2c_write_read(bus_id, addr7, sel, sizeof sel, buf, len) == OD_HAL_I2C_OK;
    }
    if (od_hal_i2c_write(bus_id, addr7, sel, sizeof sel) != OD_HAL_I2C_OK) {
        return false;
    }
    return od_hal_i2c_read(bus_id, addr7, buf, len) == OD_HAL_I2C_OK;
}

static bool gt911_read(uint8_t bus_id, uint8_t addr7, uint16_t reg, uint8_t *buf, uint8_t len,
                       bool high_first)
{
    uint8_t attempt;

    for (attempt = 0; attempt < GT911_I2C_RETRIES; attempt++) {
        if (gt911_read_once(bus_id, addr7, reg, buf, len, high_first, true)) {
            return true;
        }
        od_touch_app_delay_us(GT911_I2C_RETRY_DELAY_US);
        if (gt911_read_once(bus_id, addr7, reg, buf, len, high_first, false)) {
            return true;
        }
        od_touch_app_delay_us(GT911_I2C_RETRY_DELAY_US);
    }
    return false;
}

static bool gt911_write(uint8_t bus_id, uint8_t addr7, uint16_t reg, const uint8_t *buf,
                        uint8_t len, bool high_first)
{
    uint8_t tx[2 + 8];
    uint8_t attempt;
    uint8_t i;

    if ((uint16_t)len + 2u > sizeof tx) {
        return false;
    }
    if (high_first) {
        tx[0] = (uint8_t)(reg >> 8);
        tx[1] = (uint8_t)(reg & 0xFFu);
    } else {
        tx[0] = (uint8_t)(reg & 0xFFu);
        tx[1] = (uint8_t)(reg >> 8);
    }
    for (i = 0; i < len; i++) {
        tx[2u + i] = buf[i];
    }
    for (attempt = 0; attempt < GT911_I2C_RETRIES; attempt++) {
        if (od_hal_i2c_write(bus_id, addr7, tx, (uint16_t)(2u + len)) == OD_HAL_I2C_OK) {
            return true;
        }
        od_touch_app_delay_us(GT911_I2C_RETRY_DELAY_US);
    }
    return false;
}

/* THE STATUS BYTE MUST BE ACKNOWLEDGED. Rev.10 5 p.28: a host that does not clear 0x814E within
 * one refresh period gets "an INT pulse again instead of update coordinates", and the part "will
 * keep outputting INT pulse". So a branch that skips a sample without clearing leaves a stale
 * reading AND an interrupt storm -- which is exactly the donor's over-count wedge, FOLLOWUPS 17.
 * Every path that consumes or discards a status byte comes through here. */
static void gt911_clear_status(const struct touch_runtime *rt)
{
    uint8_t zero = 0;

    (void)gt911_write(rt->bus_id, rt->addr7, GT911_REG_STATUS, &zero, 1u, rt->reg_high_first != 0u);
}

/* Rev.10 3.3 p.14 says only "ASCII" -- the literal "911" is empirical, not documented, and
 * GT9110/GT9147-class parts answer differently. A 3-byte prefix compare is deliberate: the
 * 4-byte "911\0" compare most drivers use is stricter than anything the guide promises. */
static bool gt911_id_match(const uint8_t *id)
{
    return id[0] == '9' && id[1] == '1' && id[2] == '1';
}

/* THE DOCUMENTED BYTE ORDER IS TRIED FIRST. Rev.10 2.1 specifies Register_H then Register_L,
 * unambiguously and in both directions, and a targeted survey found no evidence of a part using
 * the other order. Both donors nevertheless probed low-byte-first first, labelling it "common".
 * Reversed here by project ruling, and the fallback is RETAINED rather than deleted: the survey
 * shows nobody documented such a part, not that none exists. DIVERGENCE_MATRIX 20. */
static bool gt911_probe(uint8_t bus_id, uint8_t addr7, uint8_t *high_first_out)
{
    uint8_t id[4];

    if (gt911_read(bus_id, addr7, GT911_REG_PID, id, sizeof id, true) && gt911_id_match(id)) {
        *high_first_out = 1u;
        return true;
    }
    if (gt911_read(bus_id, addr7, GT911_REG_PID, id, sizeof id, false) && gt911_id_match(id)) {
        *high_first_out = 0u;
        return true;
    }
    return false;
}

/* --------------------------------------------------------------------- reset and address */

/* ORDER IS THE CONTRACT. Both pads become outputs BEFORE either is driven, and INT's level at
 * RST's rising edge is what selects the address (Rev.10 4.2). Collapsing any pair into a
 * "configure output at level" call reorders that and changes which address the part answers on --
 * which is why od_touch_app keeps set_mode_output and write separate. */
static void gt911_hw_reset(const struct TouchController *t, struct touch_runtime *rt,
                           bool int_low_for_5d)
{
    if (t->rst_pin == 0xFFu) {
        return;
    }
    /* Same reason as gt911_int_wake(): this drives and reconfigures INT, which destroys the
     * trigger. Clearing here keeps the record coherent when the probe that follows FAILS and the
     * bring-up returns before reaching the re-attach.
     *
     * NO OBSERVABLE EFFECT TODAY, and deliberately kept anyway: a controller in that state is
     * !ok, every service path returns early on !ok, and a later successful re-establish repairs
     * the record. It is here so the invariant "int_attached means a live trigger" holds without
     * a reader having to prove the unreachable case. Do not go looking for the test. */
    rt->int_attached = 0u;
    if (t->int_pin == 0xFFu) {
        od_touch_app_gpio_set_mode_output(t->rst_pin);
        od_touch_app_gpio_write(t->rst_pin, false);
        od_touch_app_delay_ms(10u);
        od_touch_app_gpio_write(t->rst_pin, true);
        od_touch_app_delay_ms(60u);
        od_touch_app_gpio_set_mode_output(t->rst_pin);
        od_touch_app_gpio_write(t->rst_pin, true);
        return;
    }
    od_touch_app_delay_ms(1u);
    od_touch_app_gpio_set_mode_output(t->int_pin);
    od_touch_app_gpio_set_mode_output(t->rst_pin);
    od_touch_app_gpio_write(t->int_pin, false);
    od_touch_app_gpio_write(t->rst_pin, false);
    od_touch_app_delay_ms(GT911_RST_LOW_MS);
    od_touch_app_gpio_write(t->int_pin, !int_low_for_5d);
    od_touch_app_delay_us(GT911_INT_SETUP_US);
    od_touch_app_gpio_set_mode_output(t->rst_pin);
    od_touch_app_gpio_write(t->rst_pin, true);
    od_touch_app_delay_ms(GT911_INT_HOLD_MS);
    od_touch_app_gpio_write(t->int_pin, false);
    od_touch_app_delay_ms(GT911_INT_LOW_MS);
    od_touch_app_gpio_set_mode_output(t->rst_pin);
    od_touch_app_gpio_write(t->rst_pin, true);
    /* The guide asks for a floating input here (1 p.3, "no internal pull-up or pull-down"); both
     * donors use a pull-up and are field-proven that way. Kept, and recorded rather than
     * "corrected" from a datasheet against hardware nobody here can retest. */
    od_touch_app_gpio_config_input(t->int_pin, true);
}

/* RECONFIGURING THE PIN DESTROYS ITS INTERRUPT, so this clears the runtime's record of one.
 *
 * Both stacks do it: Zephyr's gpio_pin_configure() removes the existing trigger and frees the
 * GPIOTE channel, and IDF's gpio_config() here sets intr_type = DISABLE. Both donors nevertheless
 * re-attach only `if (!int_irq_attached)`, so after a panel refresh the flag still said attached
 * while the hardware trigger was gone -- edges stopped advancing service, leaving only the timed
 * poll and the held-low check, which is a latency regression nobody would attribute to the wake.
 * DIVERGENCE_MATRIX 23. */
static void gt911_int_wake(const struct TouchController *t, struct touch_runtime *rt)
{
    if (t->int_pin == 0xFFu) {
        return;
    }
    od_touch_app_gpio_set_mode_output(t->int_pin);
    od_touch_app_gpio_write(t->int_pin, true);
    od_touch_app_delay_ms(10u);
    od_touch_app_gpio_config_input(t->int_pin, true);
    rt->int_attached = 0u;
}

static void touch_apply_enable_pin(const struct TouchController *t)
{
    if (!valid_pin(t->enable_pin)) {
        return;
    }
    /* Active high per the header. Driven before any bus traffic: a controller behind an unasserted
     * enable does not answer its address at all, so probing first would diagnose a wiring fault. */
    od_touch_app_gpio_set_mode_output(t->enable_pin);
    od_touch_app_gpio_write(t->enable_pin, true);
}

/* A CONFIGURED ADDRESS IS NOT A HINT. When one is declared and does not answer, this returns 0
 * rather than falling through to auto-detection: auto-detect could bind a DIFFERENT controller on
 * a two-controller bus and report success. Both donors return here too. */
static uint8_t gt911_resolve(const struct TouchController *t, struct touch_runtime *rt)
{
    uint8_t want = t->i2c_addr_7bit;

    if (want != 0u && want != 0xFFu) {
        if (t->rst_pin != 0xFFu) {
            od_touch_app_delay_ms(GT911_PRE_RESET_DELAY_MS);
            gt911_hw_reset(t, rt, want == GT911_ADDR_5D);
            od_touch_app_delay_ms(GT911_POST_RESET_SETTLE_MS);
        } else {
            od_touch_app_delay_ms(10u);
        }
        if (gt911_probe(rt->bus_id, want, &rt->reg_high_first)) {
            return want;
        }
        od_log_warn("GT911: probe failed at configured addr 0x%02X", want);
        return 0u;
    }

    /* Auto-detect: one reset per candidate, because the INT level during THAT reset is what
     * selects the address. Probing both addresses after a single reset would ask 0x14 to answer
     * on a part that was just told to be 0x5D. */
    if (t->rst_pin != 0xFFu) {
        od_touch_app_delay_ms(GT911_PRE_RESET_DELAY_MS);
        gt911_hw_reset(t, rt, true);
        od_touch_app_delay_ms(GT911_POST_RESET_SETTLE_MS);
        if (gt911_probe(rt->bus_id, GT911_ADDR_5D, &rt->reg_high_first)) {
            return GT911_ADDR_5D;
        }
        od_touch_app_delay_ms(GT911_PRE_RESET_DELAY_MS);
        gt911_hw_reset(t, rt, false);
        od_touch_app_delay_ms(GT911_POST_RESET_SETTLE_MS);
        if (gt911_probe(rt->bus_id, GT911_ADDR_14, &rt->reg_high_first)) {
            return GT911_ADDR_14;
        }
    } else {
        if (gt911_probe(rt->bus_id, GT911_ADDR_5D, &rt->reg_high_first)) {
            return GT911_ADDR_5D;
        }
        if (gt911_probe(rt->bus_id, GT911_ADDR_14, &rt->reg_high_first)) {
            return GT911_ADDR_14;
        }
    }
    od_log_warn("GT911: not found on I2C (check wiring, pull-ups, RST/INT)");
    return 0u;
}

/* --------------------------------------------------------------------- coordinate map
 *
 * INSIDE THE DRIVER, AND APPLIED BEFORE CACHING AND BEFORE PACKING. Both donors do it here, and
 * it cannot move to a byte-write seam: by the time the seam sees the MSD, the sample and its
 * TouchController are gone. A driver that skips it emits perfectly well-formed bytes carrying RAW
 * controller coordinates where every host expects mapped, clipped panel pixels -- nothing in the
 * frame is malformed, so nothing anywhere reports an error. That is why the first draft's missing
 * map was a wire defect and not a cosmetic one.
 *
 * The cached last_x/last_y are MAPPED, which is what makes the latched release report the same
 * pixel the last contact did.
 */
static void apply_touch_map(const struct od_config *cfg, const struct TouchController *t,
                            uint16_t *x, uint16_t *y)
{
    uint16_t w = 0;
    uint16_t h = 0;

    if (t->flags & OD_TOUCH_FLAG_SWAP_XY) {
        uint16_t tmp = *x;

        *x = *y;
        *y = tmp;
    }
    if (t->display_instance < cfg->display_count) {
        w = cfg->displays[t->display_instance].pixel_width;
        h = cfg->displays[t->display_instance].pixel_height;
    }
    if ((t->flags & OD_TOUCH_FLAG_INVERT_X) && w > 0u) {
        *x = (w > *x) ? (uint16_t)(w - 1u - *x) : 0u;
    }
    if ((t->flags & OD_TOUCH_FLAG_INVERT_Y) && h > 0u) {
        *y = (h > *y) ? (uint16_t)(h - 1u - *y) : 0u;
    }
    if (w > 0u && *x >= w) {
        *x = (uint16_t)(w - 1u);
    }
    if (h > 0u && *y >= h) {
        *y = (uint16_t)(h - 1u);
    }
}

/* --------------------------------------------------------------------- MSD packing
 *
 * FROZEN BY CONVENTION. Byte 0's low nibble is the contact count 1..5 while touching, 6 once
 * released with the last coordinates retained in bytes 1..4, and 0 when nothing has ever been
 * touched (the whole block cleared). The high nibble is the track id. x and y are little-endian.
 * The canonical header names the block and bounds its start; this layout lives only in a donor
 * comment that three independent hosts implement, and there is no version field.
 */
static void touch_pack_msd(uint8_t start, uint8_t count, uint8_t track_id, uint16_t x, uint16_t y,
                           bool latched)
{
    /* BOUNDED HERE, EVERY POLL, not only at bring-up. The authority checks `s + 5 > 11` on each
     * poll as well as before its retained-runtime branch, and that second check is the one that
     * matters: a config write can move touch_data_start_byte while a controller is UP, and the
     * retained branch keeps that controller without re-probing. Without this the block would
     * write over whatever binary_inputs, a sensor or NFC owns at those indices and silently drop
     * the bytes past the end -- the adapters clamp, which turns a refusal into wire corruption
     * nothing reports. */
    if ((uint16_t)start + TOUCH_MSD_BLOCK > OD_MSD_DYNAMIC_LEN) {
        return;
    }
    if (count == 0u && !latched) {
        uint8_t i;

        for (i = 0; i < TOUCH_MSD_BLOCK; i++) {
            od_touch_app_msd_write((uint8_t)(start + i), 0u);
        }
        return;
    }
    od_touch_app_msd_write(start,
                           (uint8_t)(((count == 0u) ? 6u : (count & 0x0Fu)) |
                                     (uint8_t)((track_id & 0x0Fu) << 4)));
    od_touch_app_msd_write((uint8_t)(start + 1u), (uint8_t)(x & 0xFFu));
    od_touch_app_msd_write((uint8_t)(start + 2u), (uint8_t)(x >> 8));
    od_touch_app_msd_write((uint8_t)(start + 3u), (uint8_t)(y & 0xFFu));
    od_touch_app_msd_write((uint8_t)(start + 4u), (uint8_t)(y >> 8));
}

/* --------------------------------------------------------------------- lifecycle */

/* No TouchController argument: the pin to detach comes from the RUNTIME, because that is the one
 * an ISR is actually on -- the config may have moved since. */
static void touch_disable(uint8_t idx, struct touch_runtime *rt, const char *reason)
{
    if (rt->disabled) {
        return;
    }
    rt->disabled = 1u;
    rt->ok = 0u;
    if (rt->int_pin_attached != 0xFFu) {
        od_touch_app_gpio_detach_int(rt->int_pin_attached);
        rt->int_pin_attached = 0xFFu;
        rt->int_attached = 0u;
    }
    od_log_warn("Touch[%u]: disabled (%s)", idx, reason);
}

static void touch_attach_int(uint8_t idx, const struct TouchController *t,
                             struct touch_runtime *rt)
{
    /* A RECORDED PIN THAT IS NO LONGER THE CONFIGURED ONE IS DETACHED FIRST. A config write can
     * move the interrupt pin under a live controller, and nothing re-initialises touch on reload;
     * without this the old pin keeps its ISR for the rest of the boot. */
    if (rt->int_pin_attached != 0xFFu && rt->int_pin_attached != t->int_pin) {
        od_touch_app_gpio_detach_int(rt->int_pin_attached);
        rt->int_pin_attached = 0xFFu;
        rt->int_attached = 0u;
    }
    if (t->int_pin == 0xFFu || rt->int_attached) {
        return;
    }
    od_touch_app_gpio_config_input(t->int_pin, true);
    if (!od_touch_app_gpio_attach_int(idx, t->int_pin)) {
        /* NOTHING IS INSTALLED, so the record must not claim the pin. Both seams unwind a failed
         * attach completely -- ESP32 removes the handler before adding one, and Zephyr frees the
         * slot and the callback on either error exit -- and the reconfigure above destroyed the
         * previous trigger regardless. A record left naming this pin sends a later disable() or
         * init() to detach at a pin this driver no longer owns. */
        rt->int_pin_attached = 0xFFu;
        od_log_warn("Touch[%u]: IRQ attach failed on pin %u -- polling only", idx, t->int_pin);
        return;
    }
    rt->int_attached = 1u;
    rt->int_pin_attached = t->int_pin;
}

static bool touch_bring_up(uint8_t idx, const struct od_config *cfg,
                           const struct TouchController *t, struct touch_runtime *rt,
                           uint32_t now_ms)
{
    uint8_t addr;

    touch_apply_enable_pin(t);
    if (!touch_bus_ok(cfg, t)) {
        od_log_warn("Touch[%u]: no usable data_bus (bus_id %u); not probed", idx, t->bus_id);
        return false;
    }
    if (t->touch_data_start_byte > TOUCH_MSD_MAX_START) {
        od_log_warn("Touch[%u]: touch_data_start_byte %u exceeds the 5-byte window",
                    idx, t->touch_data_start_byte);
        return false;
    }
    rt->bus_id = t->bus_id;
    if (!od_touch_app_bus_prepare(rt->bus_id)) {
        return false;
    }
    addr = gt911_resolve(t, rt);
    if (addr == 0u) {
        rt->ok = 0u;
        return false;
    }
    rt->addr7 = addr;
    rt->ok = 1u;
    rt->disabled = 0u;
    rt->fail_streak = 0u;
    rt->last_fail_ms = 0u;
    rt->latched = 0u;
    rt->last_poll_ms = now_ms;
    gt911_clear_status(rt);
    if (t->int_pin != 0xFFu) {
        gt911_int_wake(t, rt);
        touch_attach_int(idx, t, rt);
    }
    {
        /* The donor reads 12 bytes from 0x8140 and logs the resolution the part reports.
         * Observability only -- nothing decodes it -- but it is the one line that tells a bench
         * operator the part is answering with sane data rather than merely ACKing its address. */
        uint8_t  info[12];
        uint16_t xres = 0;
        uint16_t yres = 0;

        if (gt911_read(rt->bus_id, addr, GT911_REG_PID, info, sizeof info,
                       rt->reg_high_first != 0u)) {
            xres = (uint16_t)((uint16_t)info[6] | ((uint16_t)info[7] << 8));
            yres = (uint16_t)((uint16_t)info[8] | ((uint16_t)info[9] << 8));
        }
        od_log_info("Touch[%u]: GT911 @0x%02X %s %ux%u%s", idx, addr,
                    rt->reg_high_first ? "BE" : "LE", xres, yres,
                    rt->int_attached ? " INT+poll" : " poll");
    }
    return true;
}

/* A WORKING CONTROLLER DOES NOT NEED ITS ADDRESS RE-SELECTED. Probe the retained address first;
 * only a failure earns the full reset and re-resolve. The first draft cleared two fields and the
 * status byte and called that a resume, which loses a controller whose bus went away under it. */
static bool touch_light_resume(uint8_t idx, const struct TouchController *t,
                               struct touch_runtime *rt)
{
    uint8_t id[4];

    if (!rt->ok || rt->addr7 == 0u) {
        return false;
    }
    if (!od_touch_app_bus_prepare(rt->bus_id)) {
        return false;
    }
    if (!gt911_read(rt->bus_id, rt->addr7, GT911_REG_PID, id, sizeof id, rt->reg_high_first != 0u) ||
        !gt911_id_match(id)) {
        return false;
    }
    rt->fail_streak = 0u;
    rt->latched = 0u;
    gt911_clear_status(rt);
    if (t->int_pin != 0xFFu) {
        gt911_int_wake(t, rt);
        touch_attach_int(idx, t, rt);
    }
    return true;
}

/* --------------------------------------------------------------------- one controller */

static uint8_t touch_interval(const struct TouchController *t)
{
    /* 0 means default, and the default is 100 ms -- NOT the 25 ms the canonical header documents.
     * Both donors use their process-loop floor here; the header's figure has never been what any
     * firmware does. DIVERGENCE_MATRIX 22. */
    return t->poll_interval_ms ? t->poll_interval_ms : TOUCH_DEFAULT_INTERVAL_MS;
}

/* Returns the delay this controller wants before it is serviced again. */
static uint32_t touch_service_one(uint8_t idx, const struct od_config *cfg,
                                  const struct TouchController *t, struct touch_runtime *rt,
                                  uint32_t now_ms, bool edge)
{
    const uint8_t interval = touch_interval(t);
    const bool    high_first = rt->reg_high_first != 0u;
    uint32_t elapsed;
    bool     line_low = false;
    bool     timed = false;
    uint8_t  status = 0;
    uint8_t  count;
    uint16_t x;
    uint16_t y;
    uint8_t  track_id = 0;
    bool     changed;

    if (!rt->ok || rt->disabled) {
        return OD_TOUCH_IDLE_MS;
    }
    if (rt->int_attached) {
        /* A held-low line means a report is waiting even though its edge was missed. Suppressed
         * during the failure backoff: a controller that is not answering also holds INT low, and
         * without this the backoff would be defeated by the very symptom it is backing off from. */
        /* THE PIN THE TRIGGER IS ON, not the one the config currently names. A config write can
         * move int_pin under a live controller with no re-init, and polling the new pin -- which
         * may belong to another subsystem and may sit low -- would make line_low true on every
         * pass, servicing this controller on every loop iteration and ignoring its interval. */
        line_low = (od_touch_app_gpio_read(rt->int_pin_attached) == 0);
        if (line_low && rt->fail_streak > 0u &&
            (uint32_t)(now_ms - rt->last_fail_ms) < TOUCH_I2C_FAIL_BACKOFF_MS) {
            line_low = false;
        }
    }
    elapsed = (uint32_t)(now_ms - rt->last_poll_ms);
    timed = elapsed >= interval;
    if (!edge && !timed && !line_low) {
        /* REMAINING, not the whole interval. Returning `interval` from a controller that is
         * partway through one pushes its own deadline out on every pass it is polled early for
         * another reason, so two controllers at 99 and 100 ms settle into ~198 ms polling. */
        return (uint32_t)(interval - elapsed);
    }
    if (edge) {
        /* BEFORE THE I2C, matching the authority. Clearing after the service walk instead leaves
         * a millisecond-wide window in which an arriving edge is discarded. */
        od_touch_app_irq_consume((uint8_t)(1u << idx));
    }
    if (!od_touch_app_bus_prepare(rt->bus_id)) {
        /* last_poll_ms is NOT stamped: nothing was polled. Stamping it would make a controller
         * whose bus is briefly unavailable wait its whole interval again instead of retrying on
         * the next pass, which is what the authority does. */
        return TOUCH_I2C_FAIL_BACKOFF_MS;
    }
    if (!gt911_read(rt->bus_id, rt->addr7, GT911_REG_STATUS, &status, 1u, high_first)) {
        if (rt->fail_streak < 255u) {
            rt->fail_streak++;
        }
        rt->last_fail_ms = now_ms;
        rt->last_poll_ms = now_ms;
        if (rt->fail_streak >= TOUCH_I2C_FAIL_DISABLE_THRESHOLD) {
            touch_disable(idx, rt, "too many I2C read failures");
            return OD_TOUCH_IDLE_MS;
        }
        if (rt->fail_streak == 1u) {
            od_log_warn("Touch[%u]: I2C read failed (status 0x814E, addr 0x%02X)", idx, rt->addr7);
        }
        return TOUCH_I2C_FAIL_BACKOFF_MS;
    }
    rt->fail_streak = 0u;

    if ((status & GT911_STATUS_BUFFER_READY) == 0u) {
        /* Not ready is not an error and must NOT be acknowledged -- bit 7 clear means the part has
         * nothing for us, and writing 0 here would race a report it is in the middle of latching. */
        if (!edge && !line_low) {
            rt->last_poll_ms = now_ms;
        }
        return interval;
    }

    count = (uint8_t)(status & 0x0Fu);
    if (count > GT911_MAX_CONTACTS) {
        /* CLEAR, THEN SKIP. The authority skips without clearing, so the part holds the same
         * nonsense byte, the next poll takes the same branch, and touch never reports again until
         * an init, a resume or a panel refresh -- plus an INT pulse every period in the meantime.
         * One glitched read is enough to enter it. Nordic clears and does not wedge; this is the
         * deliberate exception to "Firmware is the authority". FOLLOWUPS 17. */
        gt911_clear_status(rt);
        rt->last_poll_ms = now_ms;
        return interval;
    }
    rt->last_poll_ms = now_ms;

    if (count > 0u) {
        uint8_t p[8];

        /* 8-byte stride from 0x814F: id, x lo/hi, y lo/hi, size lo/hi, reserved (Rev.10 3.3).
         * Contents are little-endian even though the register ADDRESS above is big-endian. */
        if (!gt911_read(rt->bus_id, rt->addr7, GT911_REG_POINT1, p, sizeof p, high_first)) {
            if (rt->fail_streak < 255u) {
                rt->fail_streak++;
            }
            rt->last_fail_ms = now_ms;
            /* THIS THRESHOLD IS UNREACHABLE FROM HERE, and that is inherited, not intended: the
             * successful status read above resets the streak every pass, so a part whose status
             * register answers and whose point block never does retries for ever. Kept because
             * the authority does it and changing it would disable controllers the field currently
             * tolerates; recorded in FOLLOWUPS 21. */
            if (rt->fail_streak >= TOUCH_I2C_FAIL_DISABLE_THRESHOLD) {
                touch_disable(idx, rt, "too many I2C read failures");
                return OD_TOUCH_IDLE_MS;
            }
            return TOUCH_I2C_FAIL_BACKOFF_MS;
        }
        track_id = p[0];
        x = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        y = (uint16_t)((uint16_t)p[3] | ((uint16_t)p[4] << 8));
        rt->latched = 1u;
        apply_touch_map(cfg, t, &x, &y);
    } else {
        /* A release keeps the last MAPPED contact, so a host reading the release frame sees where
         * the finger left rather than the origin. */
        x = rt->last_x;
        y = rt->last_y;
        track_id = rt->last_id;
    }
    gt911_clear_status(rt);

    changed = (count != rt->last_count) ||
              (count > 0u && (x != rt->last_x || y != rt->last_y || track_id != rt->last_id));
    rt->last_count = count;
    rt->last_x = x;
    rt->last_y = y;
    rt->last_id = track_id;

    touch_pack_msd(t->touch_data_start_byte, count, track_id, x, y, rt->latched != 0u);

    if (changed) {
        /* PUBLISH ONLY ON CHANGE. The boost goes first, but not because any target here requires
         * it: ESP32's is an empty function and Nordic's publish only sets a pending flag, with the
         * payload and the interval both applied later in the same loop pass. It is ordered this
         * way because it is the order that stays correct if a target ever selects its interval
         * during the publish -- which is the nRF/Bluefruit shape, and the bug that shape produced
         * is written up in device_control.cpp. The Nordic donor published first; that is the
         * change, and it is behaviourally nil today. DIVERGENCE_MATRIX 25. */
        od_adv_app_boost();
        od_touch_app_msd_publish();
    }
    return interval;
}

/* --------------------------------------------------------------------- entry points */

uint32_t od_touch_gt911_init(const struct od_config *cfg, uint32_t now_ms)
{
    struct touch_runtime prior[OD_TOUCH_MAX_CONTROLLERS];
    uint8_t i;
    uint8_t up = 0;

    memcpy(prior, s_rt, sizeof prior);

    /* BY RECORD, NOT BY CONFIG. Walking the NEW config would miss exactly the pin that matters --
     * one a reload just moved away from, whose ISR is still installed. */
    for (i = 0; i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        if (s_rt[i].int_pin_attached != 0xFFu) {
            od_touch_app_gpio_detach_int(s_rt[i].int_pin_attached);
            s_rt[i].int_pin_attached = 0xFFu;
            s_rt[i].int_attached = 0u;
        }
    }
    /* s_suspend IS DELIBERATELY NOT CLEARED. The donor's init does not touch its suspend counter
     * either: a re-init that ran inside a refresh bracket would otherwise drop the outstanding
     * suspend and let the next poll contend with the panel for the bus. */
    for (i = 0; i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        touch_rt_clear(&s_rt[i]);
    }
    if (cfg == NULL || cfg->touch_controller_count == 0u) {
        return OD_TOUCH_IDLE_MS;
    }
    for (i = 0; i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        const struct TouchController *t = touch_cfg(cfg, i);

        if (t == NULL) {
            touch_log_unsupported(cfg, i);
            continue;
        }
        /* A CONTROLLER THAT WAS ALREADY UP IS KEPT, not re-resolved. The donor does this and it
         * is not an optimisation: the reset dance drives RST and INT, costs ~500 ms per
         * controller, and re-selects an address that was already correct. Restored after review
         * found it dropped. The IRQ is re-attached because the detach above removed it. */
        if (prior[i].ok && prior[i].addr7 != 0u &&
            t->touch_data_start_byte <= TOUCH_MSD_MAX_START) {
            s_rt[i] = prior[i];
            s_rt[i].int_attached = 0u;
            /* The loop above detached by record, so the retained pin is no longer one this driver
             * owns. Keeping it would let a later disable() detach a pin the new config does not
             * name -- and which another subsystem may hold. touch_attach_int() records the pin it
             * actually attaches. */
            s_rt[i].int_pin_attached = 0xFFu;
            s_rt[i].disabled = 0u;
            s_rt[i].fail_streak = 0u;
            /* THE NEW CONFIG'S BUS, not the retained one. Validating the new record and then
             * transacting on the old bus_id would report "kept" while talking to whatever sits at
             * that address on a bus this controller no longer uses. */
            s_rt[i].bus_id = t->bus_id;
            touch_apply_enable_pin(t);          /* the donor asserts this before the retained check */
            if (touch_bus_ok(cfg, t) && od_touch_app_bus_prepare(s_rt[i].bus_id)) {
                gt911_clear_status(&s_rt[i]);
                if (t->int_pin != 0xFFu) {
                    gt911_int_wake(t, &s_rt[i]);
                    touch_attach_int(i, t, &s_rt[i]);
                }
                s_rt[i].last_poll_ms = now_ms;
                od_log_info("Touch[%u]: kept GT911 @0x%02X", i, s_rt[i].addr7);
                up++;
                continue;
            }
            od_log_warn("Touch[%u]: bus restore failed; falling back to full bring-up", i);
            touch_rt_clear(&s_rt[i]);
        }
        if (touch_bring_up(i, cfg, t, &s_rt[i], now_ms)) {
            up++;
        } else {
            od_log_warn("Touch[%u]: init failed", i);
        }
    }
    return up ? TOUCH_DEFAULT_INTERVAL_MS : OD_TOUCH_IDLE_MS;
}

uint32_t od_touch_gt911_service(const struct od_config *cfg, uint32_t now_ms, uint8_t irq_mask)
{
    uint32_t next = OD_TOUCH_IDLE_MS;
    uint8_t  i;

    if (cfg == NULL || s_suspend > 0u) {
        /* Edges raised while the panel had the bus are stale by the time it gives it back, and the
         * resume re-acknowledges the part anyway. Discard them, or a caller that gates on a
         * non-empty mask busy-polls for the whole refresh. */
        od_touch_app_irq_consume(irq_mask);
        return OD_TOUCH_IDLE_MS;
    }
    /* Bits above the last controller index cannot be reached by the loop below and nothing else
     * clears them, so they are discarded here rather than left to spin. */
    od_touch_app_irq_consume((uint8_t)(irq_mask & (uint8_t)~((1u << OD_TOUCH_MAX_CONTROLLERS) - 1u)));

    for (i = 0; i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        const struct TouchController *t = touch_cfg(cfg, i);
        const uint8_t bit = (uint8_t)(1u << i);
        bool     edge = (irq_mask & bit) != 0u;
        uint32_t want;

        /* A BIT THIS MACHINE WILL NEVER ACT ON IS DISCARDED, NOT LEFT SET. There is no controller
         * here -- index past the count, type not GT911, or a config reload dropped it -- and its
         * ISR may still be on the old pin, re-setting the bit. The authority clears the mask
         * itself inside its disable path; this seam is how shared/ says the same thing without a
         * lock of its own. */
        if (t == NULL) {
            od_touch_app_irq_consume((uint8_t)(irq_mask & bit));
            continue;
        }
        if (s_rt[i].disabled || !s_rt[i].ok) {
            od_touch_app_irq_consume((uint8_t)(irq_mask & bit));
            continue;
        }
        want = touch_service_one(i, cfg, t, &s_rt[i], now_ms, edge);
        if (want < next) {
            next = want;
        }
    }
    return next;
}

void od_touch_gt911_suspend(void)
{
    if (s_suspend < 255u) {
        s_suspend++;
    }
}

uint32_t od_touch_gt911_resume(const struct od_config *cfg, uint32_t now_ms)
{
    /* OD_TOUCH_NO_CHANGE, not an idle delay. A caller that installs a returned delay would
     * otherwise push touch out a full second every time an unmatched resume ran -- and ESP32's
     * abortToKnownState() force-resumes on EVERY teardown, including ordinary disconnects with
     * nothing suspended. Doing nothing has to be reportable as doing nothing. */
    if (s_suspend == 0u) {
        return OD_TOUCH_NO_CHANGE;
    }
    s_suspend--;
    if (s_suspend != 0u) {
        return OD_TOUCH_NO_CHANGE;      /* still suspended: the schedule is not ours to move */
    }
    if (cfg == NULL || cfg->touch_controller_count == 0u) {
        return OD_TOUCH_IDLE_MS;
    }
    return od_touch_gt911_reestablish(cfg, now_ms);
}

uint32_t od_touch_gt911_reestablish(const struct od_config *cfg, uint32_t now_ms)
{
    uint8_t i;

    if (cfg == NULL || cfg->touch_controller_count == 0u) {
        return OD_TOUCH_IDLE_MS;
    }
    /* The panel had the bus; anything the target cached about it is stale. Dropped here rather
     * than in the caller, because only this function knows a re-establish is actually happening --
     * and before the settle, so the wait happens on a bus that is coming back up. */
    od_touch_app_bus_invalidate();
    od_touch_app_delay_ms(GT911_POST_RESET_SETTLE_MS);
    for (i = 0; i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        const struct TouchController *t = touch_cfg(cfg, i);
        struct touch_runtime *rt = &s_rt[i];

        if (t == NULL || rt->disabled) {
            continue;
        }
        if (touch_light_resume(i, t, rt)) {
            continue;
        }
        /* A controller whose `ok` is false but which was never disabled is recoverable here --
         * the full bring-up is the same path init uses. */
        if (!touch_bring_up(i, cfg, t, rt, now_ms)) {
            od_log_warn("Touch[%u]: resume failed", i);
        }
    }
    return TOUCH_DEFAULT_INTERVAL_MS;
}

uint32_t od_touch_gt911_force_resume(const struct od_config *cfg, uint32_t now_ms)
{
    if (s_suspend == 0u) {
        return OD_TOUCH_NO_CHANGE;   /* idempotent by contract, and silent about it */
    }
    /* Collapse and let the normal path do the work, so the re-establish sequence has one home. */
    s_suspend = 1u;
    return od_touch_gt911_resume(cfg, now_ms);
}

bool od_touch_gt911_is_int_pin(const struct od_config *cfg, uint8_t pin)
{
    uint8_t i;

    if (pin == 0xFFu) {
        return false;
    }
    for (i = 0; i < OD_TOUCH_MAX_CONTROLLERS; i++) {
        const struct TouchController *t = touch_cfg(cfg, i);

        if (t != NULL && t->int_pin == pin) {
            return true;
        }
    }
    return false;
}

uint8_t od_touch_gt911_address(uint8_t index)
{
    if (index >= OD_TOUCH_MAX_CONTROLLERS) {
        return 0u;
    }
    return s_rt[index].ok ? s_rt[index].addr7 : 0u;
}

#else  /* !OD_CONFIG_WITH_TOUCH */

uint32_t od_touch_gt911_init(const struct od_config *cfg, uint32_t now_ms)
{
    (void)cfg;
    (void)now_ms;
    return OD_TOUCH_IDLE_MS;
}

uint32_t od_touch_gt911_service(const struct od_config *cfg, uint32_t now_ms, uint8_t irq_mask)
{
    (void)cfg;
    (void)now_ms;
    (void)irq_mask;
    return OD_TOUCH_IDLE_MS;
}

void od_touch_gt911_suspend(void)
{
}

/* NO_CHANGE, matching the capability-on arm: "nothing was suspended, so nothing moved" is the
 * same answer here, and a caller must not install a delay for a resume that did nothing. */
uint32_t od_touch_gt911_resume(const struct od_config *cfg, uint32_t now_ms)
{
    (void)cfg;
    (void)now_ms;
    return OD_TOUCH_NO_CHANGE;
}

uint32_t od_touch_gt911_force_resume(const struct od_config *cfg, uint32_t now_ms)
{
    (void)cfg;
    (void)now_ms;
    return OD_TOUCH_NO_CHANGE;
}

uint32_t od_touch_gt911_reestablish(const struct od_config *cfg, uint32_t now_ms)
{
    (void)cfg;
    (void)now_ms;
    return OD_TOUCH_IDLE_MS;
}

bool od_touch_gt911_is_int_pin(const struct od_config *cfg, uint8_t pin)
{
    (void)cfg;
    (void)pin;
    return false;
}

uint8_t od_touch_gt911_address(uint8_t index)
{
    (void)index;
    return 0u;
}

#endif /* OD_CONFIG_WITH_TOUCH */
