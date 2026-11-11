/* od_claim_mode.h -- the security boot, the boot history (claim gesture + factory-reset pattern)
 * and the claim-mode session (SECURITY_V2 sections 7, 10.1; FACTORY_RESET.md sections 3, 5).
 *
 * ONE BOOT CALL. od_security_boot() runs, in this order, before the radio and the display start:
 *   1. the reset-pin hold check (od_factory_reset, FACTORY_RESET.md 3.2);
 *   2. the boot history: read the reset cause, update the power-cycle count, and decide
 *      FACTORY-RESET (the long pattern) / claim gesture (the short pattern) / nothing;
 *   3. a factory reset that is pending (marker) or was just triggered (pin or pattern) -- it ends
 *      in a reboot, or in LOCKED (stage 4);
 *   4. the SEC2 boot decision (od_sec2_boot, incl. the secret scan rows 5-7);
 *   5. claim-mode entry: FRESH / GESTURE / CONTINUE (section 7.1).
 * The target then starts the radio and calls od_claim_mode_tick() from its loop.
 *
 * THE BOOT HISTORY (one OS2S byte, `short_boots`): the number of consecutive previous boots that
 * were COUNTED (power-on or the chip's reset pin, od_claim_cause_counted()) and lived between
 * OD_CM_T_ARM_MS and OD_CM_T_CLEAR_MS of uptime. Any other reset, and any boot that lives past
 * OD_CM_T_CLEAR_MS, sets it back to 0. On a counted boot with k = short_boots:
 *   k >= OD_FR_PATTERN_SHORT_BOOTS   factory reset, on EVERY device (unclaimed, claimed, LOCKED)
 *   k >= OD_CM_GESTURE_SHORT_BOOTS   unclaimed: claim mode GESTURE; claimed / LOCKED: normal boot
 *   in every case below the reset   the count continues: at T_ARM write k+1, at T_CLEAR write 0
 * So the claim gesture (two quick power-cycles = one short on-period) stays what it was, and the
 * factory reset (five quick power-cycles = four short on-periods) passes through it: claim mode
 * does not swallow the count. A claim-mode boot with k >= 2 is therefore part of a reset pattern
 * in progress, and it draws its SETUP screen only once it has lived past T_CLEAR (when the
 * pattern can no longer continue), so the pattern costs no extra e-paper refresh.
 *
 * WEAR: a counted boot that lives past T_CLEAR writes OS2S twice (arm + clear), one cut between
 * T_ARM and T_CLEAR writes it once, one cut before T_ARM writes nothing. Uncounted boots write
 * only when they clear a non-zero count. One write is 16 B + the store's header.
 *
 * CONTEXT: single loop/main task.
 */
#ifndef OD_CLAIM_MODE_H
#define OD_CLAIM_MODE_H

#include <stdbool.h>
#include <stdint.h>

#include "od_sec2.h"
#include "od_sec2_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------- constants --- */

/* Compile-time. A target or board may override any of them. */
#ifndef OD_CM_WINDOW_MS
#define OD_CM_WINDOW_MS            600000u  /* claim mode lasts 10 minutes from each ENTER */
#endif
#ifndef OD_CM_MAX_RENDERS
#define OD_CM_MAX_RENDERS          3u       /* code renders per claim-mode session */
#endif
#ifndef OD_CM_MAX_RESTARTS
#define OD_CM_MAX_RESTARTS         3u       /* planned reboots that continue a session */
#endif
#ifndef OD_CM_T_ARM_MS
#define OD_CM_T_ARM_MS             2000u    /* an on-period counts from this uptime ... */
#endif
#ifndef OD_CM_T_CLEAR_MS
#define OD_CM_T_CLEAR_MS           10000u   /* ... until this one */
#endif
#ifndef OD_CM_GESTURE_SHORT_BOOTS
#define OD_CM_GESTURE_SHORT_BOOTS  1u       /* two quick power-cycles */
#endif
#ifndef OD_FR_PATTERN_SHORT_BOOTS
#define OD_FR_PATTERN_SHORT_BOOTS  4u       /* five quick power-cycles; 0 disables the pattern */
#endif
#ifndef OD_CM_END_FLUSH_MS
#define OD_CM_END_FLUSH_MS         500u     /* the END reply flush before the reboot */
#endif
#ifndef OD_CM_CODE_FLUSH_MS
#define OD_CM_CODE_FLUSH_MS        500u     /* the NONCE reply flush before the CODE render */
#endif

#if OD_FR_PATTERN_SHORT_BOOTS != 0 && OD_FR_PATTERN_SHORT_BOOTS <= OD_CM_GESTURE_SHORT_BOOTS + 1u
#error "the factory-reset pattern must be clearly longer than the claim gesture"
#endif
OD_STATIC_ASSERT(OD_CM_T_ARM_MS < OD_CM_T_CLEAR_MS, "an on-period window must not be empty");

/* ------------------------------------------------------------------- boot history --- */

/* Is this reset cause (od_hal_reset.h bitmask) a person switching power or pressing nRESET?
 * True only when every set bit is POWER_ON, PIN or POWER_ON_OR_BROWNOUT. */
bool od_claim_cause_counted(uint32_t cause);

/* The verdict of one counted / uncounted boot, for tests and the boot log. */
enum od_boot_pattern {
    OD_PATTERN_NONE = 0,       /* not counted, or no short on-period before it */
    OD_PATTERN_COUNTING,       /* a pattern is in progress (k short on-periods before this boot) */
    OD_PATTERN_CLAIM,          /* k reached the claim gesture (claim mode if unclaimed) */
    OD_PATTERN_FACTORY_RESET   /* k reached the reset pattern */
};

/* ------------------------------------------------------------------------- session --- */

enum od_claim_kind {
    OD_CLAIM_KIND_NONE = 0,
    OD_CLAIM_KIND_FRESH,
    OD_CLAIM_KIND_GESTURE
};

enum od_claim_end {
    OD_CLAIM_END_NONE = 0,
    OD_CLAIM_END_CLAIMED,
    OD_CLAIM_END_TIMEOUT,
    OD_CLAIM_END_CANCEL,       /* SEC_CLAIM EXIT before a claim */
    OD_CLAIM_END_ATTEMPTS,     /* the third code ended without a claim */
    OD_CLAIM_END_RESTART_CAP   /* a fourth planned reboot */
};

/* What od_security_boot() found, for the target's boot log and its start-up choices. */
struct od_security_boot_report {
    uint32_t              cause;          /* od_hal_reset_cause() */
    uint8_t               short_boots;    /* k, as read */
    enum od_boot_pattern  pattern;
    enum od_sec2_state    state;
    enum od_claim_kind    kind;           /* claim mode entered this boot, or NONE */
    bool                  continued;      /* CONTINUE: a session resumed across a planned reboot */
    bool                  reset_locked;   /* factory reset stage 4: LOCKED */
};

/* The whole boot sequence above. `sec` is the parsed 0x27 (NULL when no config);
 * `config_present` says a config record was loaded; `factory_provisioned` that it was provisioned
 * from factory data this boot (both feed "virgin", section 7.1). Returns only if the device boots
 * on (a factory reset reboots from inside). */
void od_security_boot(const struct SecurityConfig *sec, bool config_present,
                      bool factory_provisioned, struct od_security_boot_report *out);

/* Claim mode is active (until the reboot that ends it). */
bool od_claim_mode_active(void);

/* This boot may be one on-period of a power-cycle pattern in progress (a counted boot with a
 * non-zero count, before T_CLEAR): the target defers its normal boot-screen refresh until this
 * turns false, so the pattern costs no e-paper refresh per quick boot. */
bool od_claim_mode_defer_boot_screen(void);

/* Call every loop pass: T_ARM / T_CLEAR writes, the deadline, deferred screen draws, the END
 * flush and reboot. Draws (which may block) happen here, never inside a frame handler. */
void od_claim_mode_tick(void);

/* Only the T_ARM / T_CLEAR writes of od_claim_mode_tick(): no draw, no reboot, no session work,
 * so it is safe from inside a long blocking operation of the loop's own task -- a panel refresh
 * wait. A boot-screen or SETUP refresh that blocks the loop past T_ARM would otherwise move the
 * count to the end of the refresh (5.7 s on the reTerminal Sticky), and an on-period shorter
 * than that, such as the "wait about 5 seconds" of the user instruction (SECURITY_V2 7.2),
 * would not count. Same task only: od_claim_mode is not thread-safe. */
void od_claim_mode_tick_count(void);

/* The target is about to deep-sleep, power off or reboot for its own reasons (CMD_REBOOT, the
 * reboot that applies a config, an OTA). Clears an armed boot count first (section 7.2) and, in
 * claim mode, writes OS2C so the next boot CONTINUEs -- or, once claimed, ENDs the session.
 * Returns false when the caller must NOT sleep / power off now (claim mode stays awake). */
bool od_claim_mode_before_reboot(void);
bool od_claim_mode_before_sleep(void);

/* od_sec2_app_claim_mode() / od_sec2_app_claim_event() (od_sec2_app.h), forwarded by
 * od_sec2_hooks.c. */
void od_claim_mode_status(struct od_sec2_claim_mode *out);
void od_claim_mode_event(enum od_sec2_claim_event ev, uint32_t arg);

/* Inspection, for the boot log and tests. */
enum od_claim_kind od_claim_mode_kind(void);
enum od_claim_end od_claim_mode_end_reason(void);
uint8_t od_claim_mode_renders_used(void);
uint8_t od_claim_mode_restarts_used(void);

/* Host tests only: forget all RAM state (a "power cut"). */
void od_claim_mode_test_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CLAIM_MODE_H */
