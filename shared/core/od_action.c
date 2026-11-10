/* od_action.c -- see od_action.h. */
#include "od_action.h"

#include <limits.h>
#include <string.h>

#include "od_action_app.h"
#include "od_cfg_index.h"
#include "od_log.h"

static od_action_stats_t s_stats;

/* The entry currently running. Offsets into the packet body, not a copy: the table
 * stays in flash and is read a step at a time. */
static struct {
    bool     running;
    uint16_t entry_off;     /* body offset of the entry header */
    uint8_t  step;
    uint8_t  step_count;
    uint32_t next_ms;
} s_run;

/* Set by od_action_trigger, consumed by the service tick. Kept as a pending
 * request rather than started immediately because a trigger can arrive from a
 * GPIO edge, and starting an effect there would run panel or LED work inside an
 * interrupt-adjacent path. */
static struct {
    bool     pending;
    uint8_t  trigger;
    uint16_t arg;
} s_pend;

static const od_cfg_entry_t *table(void)
{
    return od_cfg_find(OD_PKT_ACTION_TABLE, 0u);
}

/* The one entry walk every reader shares, so the validator and the runner cannot disagree
 * about which entries exist. Reads the header at `off` into h. Returns entry_len, the step to
 * the next entry, or 0 when the walk ends (no room for a header, or an entry_len that is
 * shorter than a header or runs past the packet: nothing after it can be located). *ok is
 * false for an entry that can be stepped over but must not run: entry_len disagreeing with
 * step_count, or more than OD_ACTION_MAX_STEPS steps. */
static uint8_t entry_at(const od_cfg_entry_t *e, uint16_t off, uint8_t *h, bool *ok)
{
    if ((uint32_t)off + OD_ACTION_ENTRY_HDR > e->len ||
        od_cfg_read(e, off, h, OD_ACTION_ENTRY_HDR) != OD_ACTION_ENTRY_HDR) {
        return 0u;
    }
    if (h[0] < OD_ACTION_ENTRY_HDR || (uint32_t)off + h[0] > e->len) {
        return 0u;
    }
    *ok = h[0] == OD_ACTION_ENTRY_HDR + (uint16_t)h[5] * OD_ACTION_STEP_LEN &&
          h[5] <= OD_ACTION_MAX_STEPS;
    return h[0];
}

static bool effect_known(uint8_t e)
{
    switch (e) {
    case OD_EFF_NONE:
    case OD_EFF_LED:
    case OD_EFF_LED_STOP:
    case OD_EFF_DISPLAY_SLOT:
        return true;
    case OD_EFF_BUZZER:
        return od_action_app_has_buzzer();
    default:
        return false;
    }
}

static bool trigger_known(uint8_t t)
{
    if (t == OD_TRIG_TEMP_ENTER || t == OD_TRIG_TEMP_EXIT) {
        return od_action_app_temperature_c() != INT8_MIN;
    }
    /* BUTTON_LONG is defined but no target emits it yet: report it inactive, not active. */
    return t >= OD_TRIG_BOOT && t <= OD_TRIG_HOST && t != OD_TRIG_BUTTON_LONG;
}

/* Temperature edge state, ONE PAIR OF BITS PER TEMPERATURE ENTRY (by its ordinal among the
 * temperature entries), because each entry carries its own window.
 *
 * `known` starts clear so the first sample sets a baseline instead of firing: a device booting
 * inside its window has not entered it, and one booting outside has not left it. Without that
 * a reboot would fire every temperature action once.
 *
 * HYSTERESIS. An entry is entered at the configured edges (lo <= t <= hi) but left only once t
 * is more than OD_ACTION_TEMP_HYST_C outside them. The die reading dithers by about +/-2 C, so
 * a bare comparison turns one slow crossing into a burst of alternating ENTER/EXIT edges, each
 * of which can be a panel refresh. The band sits on the exit side so ENTER still fires exactly
 * at the edge the host configured. Entries past OD_ACTION_TEMP_MAX never fire. */
#define TEMP_POLL_MS          2000u
#define OD_ACTION_TEMP_HYST_C 3
#define OD_ACTION_TEMP_MAX    16u
static struct {
    uint16_t known;
    uint16_t inside;
    bool     polled;
    uint32_t next_poll_ms;
} s_temp;

/* Inside after sample `t`, given the previous state. */
static bool temp_inside(int8_t t, uint16_t arg, bool was_inside)
{
    const int lo = (int8_t)(arg & 0xFFu);
    const int hi = (int8_t)((arg >> 8) & 0xFFu);
    const int band = was_inside ? OD_ACTION_TEMP_HYST_C : 0;
    return (int)t >= lo - band && (int)t <= hi + band;
}

/* Poll every temperature entry and fire the ones whose window this sample just crossed. */
static void temp_poll(uint32_t now_ms)
{
    if (s_temp.polled && (int32_t)(now_ms - s_temp.next_poll_ms) < 0) {
        return;
    }
    s_temp.polled = true;
    s_temp.next_poll_ms = now_ms + TEMP_POLL_MS;

    const int8_t t = od_action_app_temperature_c();
    if (t == INT8_MIN) {
        return;
    }
    s_stats.last_temp_c = t;

    const od_cfg_entry_t *e = table();
    if (e == NULL) {
        return;
    }
    uint16_t off = 0u;
    uint8_t n = 0u;
    uint8_t len;
    uint8_t h[OD_ACTION_ENTRY_HDR];
    bool ok = false;
    while (n < OD_ACTION_TEMP_MAX && (len = entry_at(e, off, h, &ok)) != 0u) {
        const uint8_t trig = h[1];
        if (ok && (trig == OD_TRIG_TEMP_ENTER || trig == OD_TRIG_TEMP_EXIT)) {
            const uint16_t bit = (uint16_t)(1u << n);
            const uint16_t arg = (uint16_t)(h[2] | ((uint16_t)h[3] << 8));
            const bool was_in = (s_temp.inside & bit) != 0u;
            const bool now_in = temp_inside(t, arg, was_in);

            if ((s_temp.known & bit) != 0u && now_in != was_in && (h[4] & 0x01u) != 0u
                && now_in == (trig == OD_TRIG_TEMP_ENTER)) {
                od_action_trigger(trig, arg);
            }
            s_temp.known |= bit;
            s_temp.inside = now_in ? (uint16_t)(s_temp.inside | bit)
                                   : (uint16_t)(s_temp.inside & (uint16_t)~bit);
            n++;
        }
        off = (uint16_t)(off + len);
    }
}

/* Walk the table once, counting. Also the validator: a fault found here drops one
 * entry, never the table, so one bad entry cannot silence the rest -- unless its entry_len
 * cannot be trusted to find the next one (entry_at), which ends the walk. */
void od_action_reload(void)
{
    const od_cfg_entry_t *e = table();

    memset(&s_stats, 0, sizeof s_stats);
    memset(&s_run, 0, sizeof s_run);
    memset(&s_pend, 0, sizeof s_pend);
    memset(&s_temp, 0, sizeof s_temp);

    if (e == NULL) {
        return;
    }
    /* Only the first 0xB2 is run (od_cfg_find nth 0). More than one is almost always a host
     * that appended a table instead of replacing it, so count them where ACTION_INFO can
     * report it rather than shadowing the rest silently. */
    while (s_stats.tables < 0xFFu && od_cfg_find(OD_PKT_ACTION_TABLE, s_stats.tables) != NULL) {
        s_stats.tables++;
    }
    uint16_t off = 0u;
    for (;;) {
        uint8_t h[OD_ACTION_ENTRY_HDR] = { 0 };
        bool ok = false;
        const uint8_t len = entry_at(e, off, h, &ok);
        const uint8_t trig = h[1];
        const uint8_t steps = h[5];

        if (len == 0u || !ok) {
            if (len != 0u || (uint32_t)off + OD_ACTION_ENTRY_HDR <= e->len) {
                od_log_warn("action: malformed entry at +%u (len %u, steps %u)",
                            (unsigned)off, (unsigned)h[0], (unsigned)steps);
                s_stats.rejected++;
            }
            if (len == 0u) {
                break;
            }
            off = (uint16_t)(off + len);
            continue;
        }
        s_stats.entries++;

        bool runnable = trigger_known(trig);
        for (uint8_t i = 0; i < steps && runnable; i++) {
            uint8_t st[OD_ACTION_STEP_LEN];
            if (od_cfg_read(e, (uint16_t)(off + OD_ACTION_ENTRY_HDR +
                                          (uint16_t)i * OD_ACTION_STEP_LEN),
                            st, sizeof st) != sizeof st) {
                runnable = false;
                break;
            }
            if (!effect_known(st[0])) {
                runnable = false;
            }
        }
        if (runnable) {
            s_stats.active++;
        } else {
            /* Kept, not dropped. A firmware update that learns this effect should
             * start running it without the host rewriting the config. */
            s_stats.inactive++;
        }
        off = (uint16_t)(off + len);
    }

    od_log_info("action: %u entries, %u active, %u inactive, %u rejected",
                (unsigned)s_stats.entries, (unsigned)s_stats.active,
                (unsigned)s_stats.inactive, (unsigned)s_stats.rejected);
}

const od_action_stats_t *od_action_stats(void) { return &s_stats; }

void od_action_trigger(uint8_t trigger, uint16_t arg)
{
    if (trigger == OD_TRIG_NONE) {
        return;
    }
    /* Last trigger wins, and one already running is not interrupted -- see the
     * drop rule in od_action_service. */
    s_pend.pending = true;
    s_pend.trigger = trigger;
    s_pend.arg = arg;
}

/* Find the first enabled entry matching a trigger. Returns its body offset, or
 * -1. Re-read every time rather than cached: the enable bit is live, so a config
 * write takes effect on the next trigger without a reload race. */
static int32_t find_entry(uint8_t trigger, uint16_t arg, uint8_t *steps_out)
{
    const od_cfg_entry_t *e = table();
    if (e == NULL) {
        return -1;
    }
    uint16_t off = 0u;
    uint8_t len;
    uint8_t h[OD_ACTION_ENTRY_HDR];
    bool ok = false;
    while ((len = entry_at(e, off, h, &ok)) != 0u) {
        const uint16_t targ = (uint16_t)(h[2] | ((uint16_t)h[3] << 8));
        const bool enabled = (h[4] & 0x01u) != 0u;

        /* arg 0 on the entry means "any". A BUTTON_PRESS arg is a pin MASK, so it matches
         * any button in it; every other trigger matches its arg exactly. */
        const bool arg_ok = targ == 0u || targ == arg
                            || (trigger == OD_TRIG_BUTTON_PRESS && (targ & arg) != 0u);
        if (ok && enabled && h[1] == trigger && arg_ok) {
            *steps_out = h[5];
            return (int32_t)off;
        }
        off = (uint16_t)(off + len);
    }
    return -1;
}

static void run_step(const od_cfg_entry_t *e, uint16_t entry_off, uint8_t idx,
                     uint32_t now_ms)
{
    uint8_t st[OD_ACTION_STEP_LEN];

    if (od_cfg_read(e, (uint16_t)(entry_off + OD_ACTION_ENTRY_HDR +
                                  (uint16_t)idx * OD_ACTION_STEP_LEN),
                    st, sizeof st) != sizeof st) {
        s_run.running = false;
        return;
    }
    switch (st[0]) {
    case OD_EFF_LED:          od_action_app_led(st[1], st[2]); break;
    case OD_EFF_LED_STOP:     od_action_app_led_stop();        break;
    case OD_EFF_DISPLAY_SLOT: od_action_app_display_slot(st[1]); break;
    case OD_EFF_BUZZER:       od_action_app_buzzer(st[1]);     break;
    default: break;           /* OD_EFF_NONE, and anything unknown: delay only */
    }
    /* 10 ms per unit. The delay applies AFTER the effect, so a one-step entry
     * with a non-zero delay still returns promptly and does not pin the loop. */
    s_run.next_ms = now_ms + ((uint32_t)st[3] * 10u);
}

uint32_t od_action_service(uint32_t now_ms)
{
    temp_poll(now_ms);

    if (s_pend.pending && !s_run.running) {
        uint8_t steps = 0u;
        const int32_t off = find_entry(s_pend.trigger, s_pend.arg, &steps);
        s_pend.pending = false;
        if (off >= 0 && steps > 0u) {
            s_run.running = true;
            s_run.entry_off = (uint16_t)off;
            s_run.step = 0u;
            s_run.step_count = steps;
            s_run.next_ms = now_ms;
            s_stats.last_trigger = s_pend.trigger;
            s_stats.fire_count++;
        }
    } else if (s_pend.pending) {
        /* DROPPED, not queued. A one-deep queue produces the "why did it do that
         * twice, ten seconds later" behaviour that makes these systems feel
         * broken; a press during a running sequence is better ignored. */
        s_pend.pending = false;
    }

    if (!s_run.running) {
        /* Not IDLE: the temperature poll needs calling back even with nothing
         * running, or an edge is only noticed the next time something else
         * happens to wake the loop. */
        return TEMP_POLL_MS;
    }
    if ((int32_t)(now_ms - s_run.next_ms) < 0) {
        return s_run.next_ms - now_ms;
    }

    const od_cfg_entry_t *e = table();
    if (e == NULL) {
        s_run.running = false;
        return OD_ACTION_IDLE;
    }
    /* The enable bit is re-read here, not latched at trigger time: clearing it in
     * a config write must stop a sequence already in flight. */
    uint8_t h[OD_ACTION_ENTRY_HDR];
    if (od_cfg_read(e, s_run.entry_off, h, sizeof h) != sizeof h ||
        (h[4] & 0x01u) == 0u) {
        s_run.running = false;
        return OD_ACTION_IDLE;
    }

    run_step(e, s_run.entry_off, s_run.step, now_ms);
    s_run.step++;
    if (s_run.step >= s_run.step_count) {
        s_run.running = false;
        return OD_ACTION_IDLE;
    }
    /* Always a bounded return, never 0 forever: even an all-zero-delay table
     * yields between steps and finishes in step_count passes. */
    return (s_run.next_ms > now_ms) ? (s_run.next_ms - now_ms) : 0u;
}
