/* od_action.c -- see od_action.h. */
#include "od_action.h"

#include <string.h>

#include <limits.h>

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
    return t >= OD_TRIG_BOOT && t <= OD_TRIG_HOST;
}

/* Temperature edge state. `inside` starts UNKNOWN so the first sample sets a
 * baseline instead of firing: a device booting inside its window has not
 * entered it, and a device booting outside has not left it. Without that a
 * reboot would fire every temperature action once, which on a tag that
 * deep-sleeps and resets often is indistinguishable from a stuck sensor. */
#define TEMP_POLL_MS 2000u
static struct {
    bool     known;
    bool     inside;
    uint32_t next_poll_ms;
} s_temp;

/* Does `t` sit within the window packed into a trigger_arg? */
static bool temp_in_window(int8_t t, uint16_t arg)
{
    const int8_t lo = (int8_t)(arg & 0xFFu);
    const int8_t hi = (int8_t)((arg >> 8) & 0xFFu);
    return t >= lo && t <= hi;
}

/* Poll every entry with a temperature trigger and fire the ones whose window
 * this sample just crossed. Each entry carries its OWN window, so the edge is
 * per-entry rather than global -- two entries watching different ranges must
 * not share one inside/outside bit. */
static void temp_poll(uint32_t now_ms)
{
    if (s_temp.known && (int32_t)(now_ms - s_temp.next_poll_ms) < 0) {
        return;
    }
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
    while (off + OD_ACTION_ENTRY_HDR <= e->len) {
        uint8_t h[OD_ACTION_ENTRY_HDR];
        if (od_cfg_read(e, off, h, sizeof h) != sizeof h) {
            break;
        }
        const uint8_t len = h[0];
        if (len < OD_ACTION_ENTRY_HDR || (uint16_t)(off + len) > e->len) {
            break;
        }
        const uint8_t trig = h[1];
        if ((trig == OD_TRIG_TEMP_ENTER || trig == OD_TRIG_TEMP_EXIT)
            && (h[4] & 0x01u) != 0u) {
            const uint16_t arg = (uint16_t)(h[2] | ((uint16_t)h[3] << 8));
            const bool now_in = temp_in_window(t, arg);
            const bool was_in = s_temp.inside;
            if (s_temp.known && now_in != was_in) {
                if (now_in && trig == OD_TRIG_TEMP_ENTER) {
                    od_action_trigger(trig, arg);
                } else if (!now_in && trig == OD_TRIG_TEMP_EXIT) {
                    od_action_trigger(trig, arg);
                }
            }
        }
        off = (uint16_t)(off + len);
    }

    /* One shared edge bit is only correct while every temperature entry shares a
     * window. Recorded as a limitation rather than hidden: with two differing
     * windows the second would track the first's transitions. */
    const od_cfg_entry_t *first = table();
    if (first != NULL) {
        uint8_t h[OD_ACTION_ENTRY_HDR];
        uint16_t o = 0u;
        while (o + OD_ACTION_ENTRY_HDR <= first->len) {
            if (od_cfg_read(first, o, h, sizeof h) != sizeof h) break;
            if (h[0] < OD_ACTION_ENTRY_HDR) break;
            if (h[1] == OD_TRIG_TEMP_ENTER || h[1] == OD_TRIG_TEMP_EXIT) {
                s_temp.inside = temp_in_window(t, (uint16_t)(h[2] | ((uint16_t)h[3] << 8)));
                s_temp.known = true;
                break;
            }
            o = (uint16_t)(o + h[0]);
        }
    }
}

/* Walk the table once, counting. Also the validator: a fault found here drops one
 * entry, never the table, so one bad entry cannot silence the rest. */
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
    uint16_t off = 0u;
    while (off + OD_ACTION_ENTRY_HDR <= e->len) {
        uint8_t h[OD_ACTION_ENTRY_HDR];
        if (od_cfg_read(e, off, h, sizeof h) != sizeof h) {
            break;
        }
        const uint8_t len = h[0];
        const uint8_t trig = h[1];
        const uint8_t steps = h[5];

        /* entry_len is the only thing linking one entry to the next, so a value
         * that disagrees with step_count would desynchronise the whole walk, not
         * just this entry. Stop rather than guess. */
        if (len < OD_ACTION_ENTRY_HDR || (uint16_t)(off + len) > e->len ||
            len != OD_ACTION_ENTRY_HDR + (uint16_t)steps * OD_ACTION_STEP_LEN ||
            steps > OD_ACTION_MAX_STEPS) {
            od_log_warn("action: malformed entry at +%u (len %u, steps %u)",
                        (unsigned)off, (unsigned)len, (unsigned)steps);
            s_stats.rejected++;
            break;
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
    while (off + OD_ACTION_ENTRY_HDR <= e->len) {
        uint8_t h[OD_ACTION_ENTRY_HDR];
        if (od_cfg_read(e, off, h, sizeof h) != sizeof h) {
            break;
        }
        const uint8_t len = h[0];
        if (len < OD_ACTION_ENTRY_HDR || (uint16_t)(off + len) > e->len) {
            break;
        }
        const uint16_t targ = (uint16_t)(h[2] | ((uint16_t)h[3] << 8));
        const bool enabled = (h[4] & 0x01u) != 0u;

        /* arg 0 on the entry means "any": a button action that did not care which
         * pin should not have to enumerate them. */
        if (enabled && h[1] == trigger && (targ == 0u || targ == arg)) {
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
