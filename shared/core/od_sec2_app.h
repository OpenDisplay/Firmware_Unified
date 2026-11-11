/* od_sec2_app.h -- what od_sec2 needs from its surroundings, as link-time functions.
 *
 * Two groups. The first is the target's: identity, clock, panel facts, the parsed 0x27. The second
 * belongs to the claim-mode controller (od_claim_mode.c) and the config policy (od_cfg_policy.c).
 * A host test may define them all itself.
 *
 * CONTEXT: main loop only. None of these may emit a frame or call back into od_sec2.
 */
#ifndef OD_SEC2_APP_H
#define OD_SEC2_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- target ---- */

/* Milliseconds since boot (the od_hal_time clock). */
uint32_t od_sec2_app_now_ms(void);

/* The 8-byte uid of SECURITY_V2 section 4.3. MUST be stable across 3.x versions on one chip. */
void od_sec2_app_uid(uint8_t uid[8]);

/* OD_SEC2_DFLAG_HAS_PANEL | OD_SEC2_DFLAG_HAS_RESET_PIN as this device knows them now. Other
 * bits are ignored; od_sec2 adds KEY_LEGACY and CLAIM_MODE_FRESH itself. */
uint8_t od_sec2_app_dflags(void);

/* The parsed 0x27, or NULL. Only session_timeout_seconds is read after boot. */
const struct SecurityConfig *od_sec2_app_security(void);

/* A display pass is running or about to run (a queued boot / post-write redraw), so a claim
 * started now could not draw its code promptly. SEC_CLAIM BEGIN and SEC_AUTH HELLO are then
 * answered OD_SEC_ERR_BUSY at once instead of waiting behind the pass (section 8.7, BUSY). */
bool od_sec2_app_display_busy(void);

/* ---- claim mode (od_claim_mode.c) ---- */

struct od_sec2_claim_mode {
    bool     active;
    bool     fresh;          /* kind FRESH */
    bool     config_locked;  /* a code was rendered, or kind GESTURE */
    uint8_t  renders_left;   /* 3..0 */
    uint16_t window_s;       /* floor(remaining_ms / 1000) */
};

void od_sec2_app_claim_mode(struct od_sec2_claim_mode *out);

enum od_sec2_claim_event {
    OD_SEC2_EV_CODE,        /* the NONCE reply is queued: render this code now (renders_used++) */
    OD_SEC2_EV_DISCARDED,   /* a pending claim ended without success; arg = 1 if it had rendered */
    OD_SEC2_EV_EXHAUSTED,   /* ATTEMPTS_EXHAUSTED was answered: END(ATTEMPTS) */
    OD_SEC2_EV_EXIT,        /* SEC_CLAIM EXIT was answered: END(CANCEL) or END(CLAIMED) */
    OD_SEC2_EV_CLAIMED,     /* CONFIRM persisted the key */
    OD_SEC2_EV_DISCONNECTED /* od_sec2_disconnect(): the application connection is gone */
};

void od_sec2_app_claim_event(enum od_sec2_claim_event ev, uint32_t arg);

/* ---- config policy (od_cfg_policy.c) ---- */

/* SECURITY_V2 section 10.4 scrub, one atomic config rewrite. false = nothing changed. */
bool od_sec2_app_scrub(void);

/* Rewrite 0x27 at rest (section 10.2) for the current key. Best effort: a failure is repaired at
 * the next boot (section 10.1 row 2). */
void od_sec2_app_rest_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_SEC2_APP_H */
