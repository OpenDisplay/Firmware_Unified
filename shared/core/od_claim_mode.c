/* od_claim_mode.c -- the security boot, the boot history and the claim-mode session.
 * See od_claim_mode.h for the rules; SECURITY_V2 sections 7 and 10.1, FACTORY_RESET.md 3 and 5. */
#include "od_claim_mode.h"

#include <string.h>

#include "od_cfg_policy.h"
#include "od_claim_app.h"
#include "od_config_store.h"
#include "od_factory_reset.h"
#include "od_hal_reset.h"
#include "od_hal_secret.h"
#include "od_log.h"
#include "od_sec2_app.h"
#include "od_sec2_trace.h"

OD_STATIC_ASSERT(OD_CM_MAX_RENDERS >= 1u && OD_CM_MAX_RENDERS < 0xFFu, "render cap");
OD_STATIC_ASSERT(OD_CM_MAX_RESTARTS < 0xFFu, "restart cap");

/* OS2C, in retained RAM: "OS2C" | ver 1 | kind | renders | restarts | config_locked | 0 0 0 |
 * crc32 = 16 bytes. Lost on power loss, which is what ends a session on a power cut. */
#define OS2C_VER 0x01u

static struct {
    /* boot history */
    uint8_t  k;              /* short_boots as read at boot */
    bool     counted;
    bool     arm_due;        /* write k+1 at T_ARM */
    bool     armed;          /* k+1 written, the T_CLEAR write still owed */
    bool     history;        /* this boot maintains the count at all */

    /* session */
    bool     active;
    bool     claimed;
    bool     config_locked;
    bool     setup_due;
    bool     code_due;
    bool     ending;
    enum od_claim_kind kind;
    enum od_claim_end  end;
    uint8_t  renders;
    uint8_t  restarts;
    uint32_t code;
    uint32_t deadline;
    uint32_t code_at;
    uint32_t end_at;
} c;

static uint32_t now_ms(void)
{
    return od_sec2_app_now_ms();
}

static bool before(uint32_t a, uint32_t b)   /* a is before b, wrap-safe */
{
    return (int32_t)(a - b) < 0;
}

bool od_claim_cause_counted(uint32_t cause)
{
    const uint32_t ok = OD_HAL_RESET_POWER_ON | OD_HAL_RESET_PIN | OD_HAL_RESET_POWER_ON_OR_BROWNOUT;

    return cause != 0u && (cause & ~ok) == 0u;
}

/* ------------------------------------------------------------------------- OS2C --- */

static void os2c_put(void)
{
    uint8_t r[OD_HAL_RETAINED_LEN];
    uint32_t crc;

    memset(r, 0, sizeof r);
    r[0] = 'O';
    r[1] = 'S';
    r[2] = '2';
    r[3] = 'C';
    r[4] = OS2C_VER;
    r[5] = (uint8_t)c.kind;
    r[6] = c.renders;
    r[7] = c.restarts;
    r[8] = c.config_locked ? 1u : 0u;
    crc = od_config_store_crc32(r, 12u);
    r[12] = (uint8_t)(crc & 0xFFu);
    r[13] = (uint8_t)((crc >> 8) & 0xFFu);
    r[14] = (uint8_t)((crc >> 16) & 0xFFu);
    r[15] = (uint8_t)(crc >> 24);
    (void)od_hal_retained_write(r);
}

static bool os2c_get(void)
{
    uint8_t r[OD_HAL_RETAINED_LEN];
    uint32_t crc;

    if (!od_hal_retained_read(r)) {
        return false;
    }
    crc = (uint32_t)r[12] | ((uint32_t)r[13] << 8) | ((uint32_t)r[14] << 16) |
          ((uint32_t)r[15] << 24);
    if (r[0] != 'O' || r[1] != 'S' || r[2] != '2' || r[3] != 'C' || r[4] != OS2C_VER ||
        crc != od_config_store_crc32(r, 12u) ||
        (r[5] != (uint8_t)OD_CLAIM_KIND_FRESH && r[5] != (uint8_t)OD_CLAIM_KIND_GESTURE) ||
        r[6] > OD_CM_MAX_RENDERS || r[8] > 1u) {
        return false;
    }
    c.kind = (enum od_claim_kind)r[5];
    c.renders = r[6];
    c.restarts = r[7];
    c.config_locked = r[8] != 0u;
    return true;
}

/* ---------------------------------------------------------------------- the count --- */

static void count_put(uint8_t v)
{
    struct od_sec2_state_rec st;
    bool valid;

    od_sec2_os2s_get(&st, &valid);
    if (st.short_boots != v) {
        const bool ok = od_sec2_os2s_put(st.flags, v);

        OD_SEC2_TRACE("boot history: short_boots %u -> %u at %u ms (%s)", (unsigned)st.short_boots,
                      (unsigned)v, (unsigned)now_ms(), ok ? "ok" : "WRITE FAILED");
        (void)ok;
    }
}

/* The on-period ends now (sleep, power-off, planned reboot): an armed count is not a short one. */
static void count_disarm(void)
{
    if (c.armed) {
        count_put(0u);
        c.armed = false;
    }
    c.arm_due = false;
}

static void clear_fresh(void)
{
    struct od_sec2_state_rec st;
    bool valid;

    od_sec2_os2s_get(&st, &valid);
    if ((st.flags & OD_SEC2_OS2S_FRESH) != 0u) {
        (void)od_sec2_os2s_put((uint8_t)(st.flags & ~OD_SEC2_OS2S_FRESH), st.short_boots);
    }
}

/* -------------------------------------------------------------------- the session --- */

static void end_session(enum od_claim_end why)
{
    if (c.ending) {
        return;
    }
    c.ending = true;
    c.end = why;
    c.setup_due = false;
    c.code_due = false;
    c.end_at = now_ms() + OD_CM_END_FLUSH_MS;
    od_hal_retained_clear();
    clear_fresh();                       /* Q15: whatever the reason */
    od_log_info("claim mode ends: reason %u", (unsigned)why);
    OD_SEC2_TRACE("claim mode END reason=%u (1 CLAIMED 2 TIMEOUT 3 CANCEL 4 ATTEMPTS 5 RESTART_CAP) "
                  "renders=%u restarts=%u; reboot in %u ms", (unsigned)why, (unsigned)c.renders,
                  (unsigned)c.restarts, (unsigned)OD_CM_END_FLUSH_MS);
}

static void enter(enum od_claim_kind kind, bool resumed)
{
    c.active = true;
    c.kind = kind;
    if (!resumed) {
        c.renders = 0u;
        c.restarts = 0u;
        c.config_locked = kind == OD_CLAIM_KIND_GESTURE;
    }
    c.deadline = now_ms() + OD_CM_WINDOW_MS;
    c.setup_due = true;
    od_claim_app_signal(OD_CLAIM_SIG_CM_WAITING, 0u);
    od_log_info("claim mode: kind %u%s", (unsigned)kind, resumed ? " (continued)" : "");
    OD_SEC2_TRACE("claim mode ENTER kind=%s%s renders=%u restarts=%u config_locked=%u window=%u ms",
                  kind == OD_CLAIM_KIND_FRESH ? "FRESH" : "GESTURE", resumed ? " (CONTINUE)" : "",
                  (unsigned)c.renders, (unsigned)c.restarts, (unsigned)c.config_locked,
                  (unsigned)OD_CM_WINDOW_MS);
}

void od_claim_mode_status(struct od_sec2_claim_mode *out)
{
    memset(out, 0, sizeof *out);
    if (!c.active) {
        return;
    }
    out->active = true;
    out->fresh = c.kind == OD_CLAIM_KIND_FRESH;
    out->config_locked = c.config_locked || c.ending;
    out->renders_left = (uint8_t)(c.renders >= OD_CM_MAX_RENDERS ? 0u : OD_CM_MAX_RENDERS - c.renders);
    if (!c.ending && before(now_ms(), c.deadline)) {
        out->window_s = (uint16_t)((c.deadline - now_ms()) / 1000u);
    }
}

void od_claim_mode_event(enum od_sec2_claim_event ev, uint32_t arg)
{
    if (!c.active || c.ending) {
        return;
    }
    switch (ev) {
    case OD_SEC2_EV_CODE: {
        const uint32_t until = now_ms() + OD_SEC2_NONCE_TO_CONFIRM_MS;

        if (c.renders < 0xFFu) {
            ++c.renders;
        }
        c.config_locked = true;
        c.code = arg;
        c.code_due = true;
        c.setup_due = false;
        c.code_at = now_ms() + OD_CM_CODE_FLUSH_MS;
        if (before(c.deadline, until)) {
            c.deadline = until;          /* a pending claim extends the window to its CONFIRM */
        }
        od_claim_app_signal(OD_CLAIM_SIG_CM_CODE, 0u);
        OD_SEC2_TRACE("claim CODE %03u %03u (render %u of %u), CODE screen due in %u ms",
                      (unsigned)(arg / 1000u), (unsigned)(arg % 1000u), (unsigned)c.renders,
                      (unsigned)OD_CM_MAX_RENDERS, (unsigned)OD_CM_CODE_FLUSH_MS);
        break;
    }
    case OD_SEC2_EV_DISCARDED:
        OD_SEC2_TRACE("claim discarded (nonced=%u), renders=%u", (unsigned)arg, (unsigned)c.renders);
        if (c.renders >= OD_CM_MAX_RENDERS) {
            end_session(OD_CLAIM_END_ATTEMPTS);
        } else {
            od_claim_app_signal(OD_CLAIM_SIG_CM_WAITING, 0u);
        }
        break;
    case OD_SEC2_EV_EXHAUSTED:
        OD_SEC2_TRACE("claim attempts exhausted");
        end_session(OD_CLAIM_END_ATTEMPTS);
        break;
    case OD_SEC2_EV_EXIT:
        OD_SEC2_TRACE("claim EXIT (claimed=%u)", (unsigned)c.claimed);
        end_session(c.claimed ? OD_CLAIM_END_CLAIMED : OD_CLAIM_END_CANCEL);
        break;
    case OD_SEC2_EV_CLAIMED:
        OD_SEC2_TRACE("claim CONFIRMED: device CLAIMED (still in claim mode until it ends)");
        c.claimed = true;
        c.code_due = false;
        od_claim_app_signal(OD_CLAIM_SIG_CM_CLAIMED, 0u);
        break;
    case OD_SEC2_EV_DISCONNECTED:
        OD_SEC2_TRACE("claim-mode peer disconnected (claimed=%u)", (unsigned)c.claimed);
        if (c.claimed) {
            end_session(OD_CLAIM_END_CLAIMED);
        }
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------------- the boot --- */

static enum od_boot_pattern pattern_of(uint8_t k, bool counted)
{
    if (!counted || k == 0u) {
        return OD_PATTERN_NONE;
    }
#if OD_FR_PATTERN_SHORT_BOOTS != 0
    if (k >= OD_FR_PATTERN_SHORT_BOOTS) {
        return OD_PATTERN_FACTORY_RESET;
    }
#endif
    return k >= OD_CM_GESTURE_SHORT_BOOTS ? OD_PATTERN_CLAIM : OD_PATTERN_COUNTING;
}

static uint8_t read_k(void)
{
    uint8_t rec[OD_SEC2_OS2S_LEN];
    struct od_sec2_state_rec st;

    if (od_hal_secret_read(OD_HAL_SECRET_STATE, rec, OD_SEC2_OS2S_LEN) &&
        od_sec2_os2s_decode(rec, &st)) {
        return st.short_boots;
    }
    return 0u;
}

void od_security_boot(const struct SecurityConfig *sec, bool config_present,
                      bool factory_provisioned, struct od_security_boot_report *out)
{
    struct od_security_boot_report rep;
    enum od_fr_pin_verdict pin;
    enum od_fr_result fr = OD_FR_NONE;
    struct od_sec2_state_rec st;
    bool st_valid;
    bool has_secrets;

    memset(&rep, 0, sizeof rep);
    memset(&c, 0, sizeof c);

    /* 1. the reset pin (FACTORY_RESET.md 3.1 step 3) */
    pin = od_fr_pin_check(sec);

    /* 2. the boot history */
    rep.cause = od_hal_reset_cause();
    c.counted = od_claim_cause_counted(rep.cause);
    c.k = read_k();
    rep.short_boots = c.k;
    rep.pattern = pattern_of(c.k, c.counted);
    OD_SEC2_TRACE("boot: cause=0x%03x counted=%u short_boots(k)=%u pattern=%u "
                  "(0 NONE 1 COUNTING 2 CLAIM 3 FACTORY_RESET) reset_pin=%u",
                  (unsigned)rep.cause, (unsigned)c.counted, (unsigned)c.k, (unsigned)rep.pattern,
                  (unsigned)pin);

    /* 3. a factory reset: pending, or triggered by the pin or the pattern */
    if (od_fr_pending(NULL)) {
        fr = od_fr_resume();
    } else if (pin == OD_FR_PIN_COMMIT || rep.pattern == OD_PATTERN_FACTORY_RESET) {
        od_log_warn("factory reset: %s", pin == OD_FR_PIN_COMMIT ? "reset pin" : "power-cycle pattern");
        fr = od_fr_start();
    }
    if (fr == OD_FR_REBOOT) {
        if (out != NULL) {
            *out = rep;
        }
        od_hal_reset_reboot();
        return;   /* host tests only: the reboot returns */
    }

    /* 4. the boot decision */
    if (fr == OD_FR_LOCKED) {
        (void)od_sec2_boot_ex(sec, false);
        od_sec2_force_locked();
        rep.reset_locked = true;
    } else {
        (void)od_cfg_policy_zero_stale_key();
        has_secrets = od_cfg_policy_stored_has_secrets();
        (void)od_sec2_boot_ex(sec, has_secrets);
        if (od_sec2_state() == OD_SEC2_CLAIMED) {
            od_cfg_policy_rest_sync();   /* 10.1 row 2 repair, idempotent */
        }
    }
    rep.state = od_sec2_state();

    /* the count: every device keeps it (the reset pattern works on claimed and LOCKED ones) */
    c.history = OD_FR_PATTERN_SHORT_BOOTS != 0u || rep.state == OD_SEC2_UNCLAIMED;
    if (!c.counted || !c.history) {
        if (c.k != 0u) {
            count_put(0u);
        }
    } else {
        c.arm_due = true;
    }

    /* 5. claim-mode entry (UNCLAIMED only) */
    od_sec2_os2s_get(&st, &st_valid);
    if (rep.state != OD_SEC2_UNCLAIMED || rep.reset_locked) {
        od_hal_retained_clear();
    } else if (rep.cause == OD_HAL_RESET_SOFTWARE && os2c_get()) {
        rep.continued = true;
        ++c.restarts;
        enter(c.kind, true);
        if (c.restarts > OD_CM_MAX_RESTARTS) {
            end_session(OD_CLAIM_END_RESTART_CAP);
        } else {
            os2c_put();
        }
    } else {
        od_hal_retained_clear();
        if (!st_valid && (!config_present || factory_provisioned)) {
            /* virgin */
            (void)od_sec2_os2s_put((uint8_t)(st.flags | OD_SEC2_OS2S_FRESH | OD_SEC2_OS2S_MIGRATED),
                                   st.short_boots);
            od_sec2_os2s_get(&st, &st_valid);
        }
        if ((st.flags & OD_SEC2_OS2S_FRESH) != 0u) {
            enter(OD_CLAIM_KIND_FRESH, false);
        } else if (rep.pattern == OD_PATTERN_CLAIM) {
            enter(OD_CLAIM_KIND_GESTURE, false);
        }
    }
    rep.kind = c.active ? c.kind : OD_CLAIM_KIND_NONE;
    OD_SEC2_TRACE("boot decision: state=%u (0 UNCLAIMED 1 CLAIMED 2 LOCKED) os2s_valid=%u flags=0x%02x "
                  "key_gen=%u fr=%u claim_kind=%u history=%u arm_due=%u",
                  (unsigned)rep.state, (unsigned)st_valid, (unsigned)st.flags, (unsigned)st.key_gen,
                  (unsigned)fr, (unsigned)rep.kind, (unsigned)c.history, (unsigned)c.arm_due);
    if (out != NULL) {
        *out = rep;
    }
}

bool od_claim_mode_active(void)
{
    return c.active;
}

bool od_claim_mode_defer_boot_screen(void)
{
    /* A counted boot with a count running may be one step of a pattern: its refresh waits until
     * the on-period is no longer short. */
    return c.counted && c.history && c.k != 0u && (c.arm_due || c.armed);
}

void od_claim_mode_tick(void)
{
    const uint32_t t = now_ms();

    if (c.arm_due && !before(t, OD_CM_T_ARM_MS)) {
        c.arm_due = false;
        if (before(t, OD_CM_T_CLEAR_MS)) {
            count_put((uint8_t)(c.k < 0xFEu ? c.k + 1u : 0xFFu));
            c.armed = true;
            od_claim_app_signal(OD_CLAIM_SIG_PATTERN_COUNT, (uint8_t)(c.k + 1u));
        }
    }
    if (c.armed && !before(t, OD_CM_T_CLEAR_MS)) {
        c.armed = false;
        count_put(0u);
    }
    if (!c.active) {
        return;
    }
    if (c.ending) {
        if (!before(t, c.end_at)) {
            OD_SEC2_TRACE("claim mode over: software reset into normal mode");
            c.active = false;
            od_hal_reset_reboot();
        }
        return;
    }
    if (!before(t, c.deadline)) {
        end_session(c.claimed ? OD_CLAIM_END_CLAIMED : OD_CLAIM_END_TIMEOUT);
        return;
    }
    if (c.code_due && !before(t, c.code_at)) {
        c.code_due = false;
        OD_SEC2_TRACE("draw CODE screen (code %03u %03u)", (unsigned)(c.code / 1000u),
                      (unsigned)(c.code % 1000u));
        (void)od_claim_app_draw(OD_SETUP_SCREEN_CODE, c.code);
        return;
    }
    /* SETUP waits out a pattern in progress (k >= 2), so the pattern costs no extra refresh. */
    if (c.setup_due && !c.claimed && (c.k < 2u || !before(t, OD_CM_T_CLEAR_MS))) {
        c.setup_due = false;
        OD_SEC2_TRACE("draw SETUP screen");
        (void)od_claim_app_draw(OD_SETUP_SCREEN_SETUP, 0u);
    }
}

bool od_claim_mode_before_reboot(void)
{
    count_disarm();
    if (c.active && !c.ending) {
        if (c.claimed) {
            c.ending = true;
            c.end = OD_CLAIM_END_CLAIMED;
            od_hal_retained_clear();
            clear_fresh();
        } else {
            os2c_put();
            OD_SEC2_TRACE("planned reboot in claim mode: OS2C written (renders=%u restarts=%u)",
                          (unsigned)c.renders, (unsigned)c.restarts);
        }
    }
    return true;
}

bool od_claim_mode_before_sleep(void)
{
    if (c.active) {
        return false;
    }
    count_disarm();
    return true;
}

enum od_claim_kind od_claim_mode_kind(void)
{
    return c.active ? c.kind : OD_CLAIM_KIND_NONE;
}

enum od_claim_end od_claim_mode_end_reason(void)
{
    return c.end;
}

uint8_t od_claim_mode_renders_used(void)
{
    return c.renders;
}

uint8_t od_claim_mode_restarts_used(void)
{
    return c.restarts;
}

void od_claim_mode_test_reset(void)
{
    memset(&c, 0, sizeof c);
}
