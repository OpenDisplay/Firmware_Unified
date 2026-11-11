/* od_factory_reset.c -- FACTORY_RESET.md sections 3.2 and 5. See od_factory_reset.h. */
#include "od_factory_reset.h"

#include <string.h>

#include "od_cfg_policy.h"
#include "od_claim_app.h"
#include "od_config_store.h"
#include "od_hal_reset.h"
#include "od_hal_secret.h"
#include "od_log.h"
#include "od_sec2.h"
#include "od_sec2_app.h"

/* ------------------------------------------------------------------ the pin check --- */

void od_fr_pin_init(struct od_fr_pin *p)
{
    memset(p, 0, sizeof *p);
}

enum od_fr_pin_verdict od_fr_pin_step(struct od_fr_pin *p, bool asserted, uint32_t now_ms)
{
    if (!p->started) {
        p->started = true;
        if (!asserted) {
            return OD_FR_PIN_NORMAL;          /* the first read: not held, no delay */
        }
        p->t_start = now_ms;                 /* the first asserted sample */
        p->level = false;
        p->run = 1u;
        p->t_run = now_ms;
        return OD_FR_PIN_RUNNING;
    }
    if (asserted != p->level) {
        if (p->run == 0u) {
            p->t_run = now_ms;
        }
        if (p->run < 0xFFu) {
            ++p->run;
        }
    } else {
        p->run = 0u;
    }
    if (!p->level) {
        if (p->run >= OD_FR_DEBOUNCE_N) {
            p->level = true;                 /* debounced asserted; the hold runs from t_start */
            p->run = 0u;
        } else if (p->run == 0u) {
            return OD_FR_PIN_NORMAL;         /* a glitch, released before it settled */
        }
    } else {
        if (p->run >= OD_FR_DEBOUNCE_N) {
            const uint32_t held = (uint32_t)(p->t_run - p->t_start);

            return (held >= OD_FR_HOLD_MS && held < OD_FR_STUCK_MS) ? OD_FR_PIN_COMMIT
                                                                    : OD_FR_PIN_NORMAL;
        }
        if ((uint32_t)(now_ms - p->t_start) >= OD_FR_HOLD_MS) {
            p->threshold = true;
        }
    }
    /* Not while a release is being confirmed: a release that began before 30 s commits. */
    if (p->run == 0u && (uint32_t)(now_ms - p->t_start) >= OD_FR_STUCK_MS) {
        return OD_FR_PIN_STUCK;
    }
    return OD_FR_PIN_RUNNING;
}

enum od_fr_pin_verdict od_fr_pin_check(const struct SecurityConfig *sec)
{
    struct od_fr_pin p;
    enum od_fr_pin_verdict v;
    bool high;
    bool signalled_hold = false;
    bool signalled_release = false;
    uint32_t t = 0u;

    if (sec == NULL || (sec->flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) == 0u) {
        return OD_FR_PIN_NORMAL;
    }
    high = (sec->flags & OD_SECURITY_FLAG_RESET_PIN_POLARITY) != 0u;
    if (!od_claim_app_pin_input(sec->reset_pin,
                                (sec->flags & OD_SECURITY_FLAG_RESET_PIN_PULLUP) != 0u,
                                (sec->flags & OD_SECURITY_FLAG_RESET_PIN_PULLDOWN) != 0u)) {
        return OD_FR_PIN_NORMAL;
    }
    od_claim_app_wait_ms(1u);
    od_fr_pin_init(&p);
    for (;;) {
        const int raw = od_claim_app_pin_read(sec->reset_pin);
        const bool asserted = raw >= 0 && ((raw != 0) == high);

        v = od_fr_pin_step(&p, asserted, t);
        if (v != OD_FR_PIN_RUNNING) {
            break;
        }
        if (p.level && !signalled_hold) {
            od_claim_app_signal(OD_CLAIM_SIG_FR_HOLDING, 0u);
            signalled_hold = true;
        }
        if (p.threshold && !signalled_release) {
            od_claim_app_signal(OD_CLAIM_SIG_FR_RELEASE_NOW, 0u);
            signalled_release = true;
        }
        od_claim_app_wait_ms(OD_FR_SAMPLE_MS);
        t += OD_FR_SAMPLE_MS;
    }
    if (v == OD_FR_PIN_STUCK) {
        od_log_warn("reset pin stuck: ignored this boot");
        od_claim_app_signal(OD_CLAIM_SIG_FR_STUCK, 0u);
    } else if (signalled_hold && v == OD_FR_PIN_NORMAL) {
        od_claim_app_signal(OD_CLAIM_SIG_NONE, 0u);
    }
    return v;
}

/* --------------------------------------------------------------------- the marker --- */

static uint32_t crc32_of(const uint8_t *p, uint32_t n)
{
    return od_config_store_crc32(p, n);
}

void od_fr_marker_encode(const struct od_fr_marker *m, uint8_t out[OD_FR_OS2R_LEN])
{
    uint32_t c;

    out[0] = 'O';
    out[1] = 'S';
    out[2] = '2';
    out[3] = 'R';
    out[4] = 0x02u;
    out[5] = m->stage;
    out[6] = m->draw_attempts;
    out[7] = m->erase_attempts;
    c = crc32_of(out, 8u);
    out[8] = (uint8_t)(c & 0xFFu);
    out[9] = (uint8_t)((c >> 8) & 0xFFu);
    out[10] = (uint8_t)((c >> 16) & 0xFFu);
    out[11] = (uint8_t)(c >> 24);
}

bool od_fr_marker_decode(const uint8_t in[OD_FR_OS2R_LEN], struct od_fr_marker *m)
{
    const uint32_t c = (uint32_t)in[8] | ((uint32_t)in[9] << 8) | ((uint32_t)in[10] << 16) |
                       ((uint32_t)in[11] << 24);

    if (in[0] != 'O' || in[1] != 'S' || in[2] != '2' || in[3] != 'R' || in[4] != 0x02u ||
        c != crc32_of(in, 8u) || in[5] < OD_FR_STAGE_SCRUB || in[5] > OD_FR_STAGE_KEY_FAILED) {
        return false;
    }
    m->stage = in[5];
    m->draw_attempts = in[6];
    m->erase_attempts = in[7];
    return true;
}

static bool marker_put(const struct od_fr_marker *m)
{
    uint8_t rec[OD_FR_OS2R_LEN];

    od_fr_marker_encode(m, rec);
    return od_hal_secret_write(OD_HAL_SECRET_MARKER, rec, OD_FR_OS2R_LEN);
}

bool od_fr_pending(struct od_fr_marker *out)
{
    uint8_t rec[OD_FR_OS2R_LEN];
    struct od_fr_marker m;

    if (!od_hal_secret_read(OD_HAL_SECRET_MARKER, rec, OD_FR_OS2R_LEN) ||
        !od_fr_marker_decode(rec, &m)) {
        return false;
    }
    if (out != NULL) {
        *out = m;
    }
    return true;
}

/* -------------------------------------------------------------------- the stages --- */

static bool os2s_read(struct od_sec2_state_rec *st)
{
    uint8_t rec[OD_SEC2_OS2S_LEN];

    if (od_hal_secret_read(OD_HAL_SECRET_STATE, rec, OD_SEC2_OS2S_LEN) &&
        od_sec2_os2s_decode(rec, st)) {
        return true;
    }
    memset(st, 0, sizeof *st);
    return false;
}

static bool os2s_write(const struct od_sec2_state_rec *st)
{
    uint8_t rec[OD_SEC2_OS2S_LEN];

    od_sec2_os2s_encode(st, rec);
    return od_hal_secret_write(OD_HAL_SECRET_STATE, rec, OD_SEC2_OS2S_LEN);
}

/* R2: the key_gen tombstone first (so a returning OS2K is caught by 10.1 row 1 even if R6 is
 * never reached), then the key. */
static bool erase_key(void)
{
    uint8_t rec[OD_SEC2_OS2K_LEN];
    struct od_sec2_key_rec k;
    struct od_sec2_state_rec st;

    if (od_hal_secret_read(OD_HAL_SECRET_KEY, rec, OD_SEC2_OS2K_LEN) &&
        od_sec2_os2k_decode(rec, &k)) {
        (void)os2s_read(&st);
        if (st.key_gen <= k.gen) {
            st.key_gen = (uint16_t)(k.gen + 1u);
            if (!os2s_write(&st)) {
                memset(rec, 0, sizeof rec);
                memset(&k, 0, sizeof k);
                return false;
            }
        }
    }
    memset(rec, 0, sizeof rec);
    memset(&k, 0, sizeof k);
    od_hal_retained_clear();
    od_sec2_teardown();
    return od_hal_secret_erase(OD_HAL_SECRET_KEY);
}

static enum od_fr_result run(struct od_fr_marker m)
{
    struct od_sec2_state_rec st;

    od_claim_app_signal(OD_CLAIM_SIG_FR_ERASING, 0u);
    if (m.stage == OD_FR_STAGE_KEY_FAILED) {
        od_claim_app_signal(OD_CLAIM_SIG_FR_LOCKED, 0u);
        return OD_FR_LOCKED;
    }
    if (m.stage == OD_FR_STAGE_SCRUB) {
        /* R1b. A scrub that fails drops the whole record instead: no secret may outlive R2. */
        if (!od_cfg_policy_scrub() && od_config_store_clear() != OD_CONFIG_STORE_OK) {
            ++m.erase_attempts;
            if (m.erase_attempts >= OD_FR_MAX_ERASE_FAILS) {
                m.stage = OD_FR_STAGE_KEY_FAILED;
            }
            (void)marker_put(&m);
            return m.stage == OD_FR_STAGE_KEY_FAILED ? OD_FR_LOCKED : OD_FR_REBOOT;
        }
        /* R2 */
        if (!erase_key()) {
            ++m.erase_attempts;
            if (m.erase_attempts >= OD_FR_MAX_ERASE_FAILS) {
                m.stage = OD_FR_STAGE_KEY_FAILED;
                (void)marker_put(&m);
                od_log_error("factory reset: key erase failed 3 times, LOCKED");
                od_claim_app_signal(OD_CLAIM_SIG_FR_LOCKED, 0u);
                return OD_FR_LOCKED;
            }
            (void)marker_put(&m);
            return OD_FR_REBOOT;
        }
        m.stage = OD_FR_STAGE_DRAW;
    }
    if (m.stage == OD_FR_STAGE_DRAW) {
        /* R3: count the attempt before drawing, so a brown-out during the refresh counts. */
        if (m.draw_attempts < 0xFFu) {
            ++m.draw_attempts;
        }
        if (!marker_put(&m)) {
            return OD_FR_REBOOT;
        }
        if (m.draw_attempts <= OD_FR_MAX_DRAWS) {
            (void)od_claim_app_draw(OD_SETUP_SCREEN_RESET, 0u);
        }
        /* R4 */
        m.stage = OD_FR_STAGE_ERASE_REST;
        if (!marker_put(&m)) {
            return OD_FR_REBOOT;
        }
    }
    /* R5a-c: each idempotent. Only the config erase can hold the reset up: the device is already
     * keyless and secret-free, so after 3 failures it goes on to R6 rather than loop. */
    (void)od_claim_app_erase_images();
    (void)od_claim_app_erase_bonds();
    if (od_config_store_clear() != OD_CONFIG_STORE_OK) {
        ++m.erase_attempts;
        if (m.erase_attempts < OD_FR_MAX_ERASE_FAILS) {
            (void)marker_put(&m);
            return OD_FR_REBOOT;
        }
        od_log_error("factory reset: config erase failed, continuing");
    }
    /* R6 */
    (void)os2s_read(&st);
    st.flags = (uint8_t)(OD_SEC2_OS2S_FRESH | OD_SEC2_OS2S_MIGRATED);
    st.short_boots = 0u;
    st.key_gen = (uint16_t)(st.key_gen + 1u);
    if (!os2s_write(&st)) {
        return OD_FR_REBOOT;
    }
    /* R7, R8 */
    (void)od_hal_secret_erase(OD_HAL_SECRET_MARKER);
    od_claim_app_signal(OD_CLAIM_SIG_FR_DONE, 0u);
    return OD_FR_REBOOT;
}

enum od_fr_result od_fr_start(void)
{
    struct od_fr_marker m = { OD_FR_STAGE_SCRUB, 0u, 0u };

    /* R1. A lost write means no reset happened yet: the user repeats the trigger. */
    if (!marker_put(&m)) {
        od_log_error("factory reset: marker write failed");
        return OD_FR_NONE;
    }
    return run(m);
}

enum od_fr_result od_fr_resume(void)
{
    struct od_fr_marker m;

    if (!od_fr_pending(&m)) {
        return OD_FR_NONE;
    }
    return run(m);
}
