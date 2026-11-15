/* od_factory_reset.h -- the reset-pin hold check and the power-fail-safe erase
 * (FACTORY_RESET.md sections 3, 5; the power-cycle pattern trigger is od_claim_mode.h).
 *
 * Both triggers -- the 0x27 reset pin held 5-30 s, and five quick power-cycles -- run the same
 * ordered erase, R1 .. R8, made resumable by the OS2R marker:
 *   R1  marker {SCRUB}                 R4  marker {ERASE_REST}
 *   R1b scrub secrets (one rewrite)    R5a image store, R5b bonds + extras, R5c config record
 *   R2  erase OS2K (3 failures: LOCKED, stage 4)
 *   R3  marker {DRAW, draws+1}; draw RESET if draws <= 2 and mode 0 is drawable
 *   R6  OS2S {FRESH, MIGRATED, key_gen tombstone}     R7 delete marker     R8 reboot
 * Invariant (A4), host-tested at every interruption point: no point leaves an unclaimed device
 * with a secret in its config, even when the marker is lost there.
 *
 * CONTEXT: boot only, before the radio. Blocking.
 */
#ifndef OD_FACTORY_RESET_H
#define OD_FACTORY_RESET_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ the pin check --- */

#define OD_FR_SAMPLE_MS     10u
#define OD_FR_DEBOUNCE_N    5u       /* 5 equal samples = 50 ms */
#define OD_FR_HOLD_MS       5000u
#define OD_FR_STUCK_MS      30000u

enum od_fr_pin_verdict {
    OD_FR_PIN_RUNNING = 0,   /* keep sampling */
    OD_FR_PIN_NORMAL,        /* not asserted, or released before 5 s: normal boot */
    OD_FR_PIN_COMMIT,        /* released after 5 s and before 30 s: factory reset */
    OD_FR_PIN_STUCK          /* still asserted at 30 s: ignore the pin this boot */
};

/* The debounce / hold machine, fed one raw sample (asserted or not) per OD_FR_SAMPLE_MS. Pure,
 * so a host test drives it with scripted traces. */
struct od_fr_pin {
    bool     started;
    bool     level;          /* debounced: asserted */
    bool     threshold;      /* the 5 s threshold was reached (signalled once) */
    uint8_t  run;            /* consecutive samples that differ from `level` */
    uint32_t t_start;        /* first asserted sample */
    uint32_t t_run;          /* first sample of the current differing run */
};

void od_fr_pin_init(struct od_fr_pin *p);
enum od_fr_pin_verdict od_fr_pin_step(struct od_fr_pin *p, bool asserted, uint32_t now_ms);

/* The whole check for the 0x27 in `sec` (NULL or pin disabled: NORMAL at once, no delay). Drives
 * od_claim_app_pin_* / od_claim_app_wait_ms and the LED patterns. */
enum od_fr_pin_verdict od_fr_pin_check(const struct SecurityConfig *sec);

/* --------------------------------------------------------------------- the erase --- */

#define OD_FR_STAGE_SCRUB      1u
#define OD_FR_STAGE_DRAW       2u
#define OD_FR_STAGE_ERASE_REST 3u
#define OD_FR_STAGE_KEY_FAILED 4u

#define OD_FR_MAX_DRAWS        2u
#define OD_FR_MAX_ERASE_FAILS  3u

struct od_fr_marker {
    uint8_t stage;
    uint8_t draw_attempts;
    uint8_t erase_attempts;
};

#define OD_FR_OS2R_LEN 12u
void od_fr_marker_encode(const struct od_fr_marker *m, uint8_t out[OD_FR_OS2R_LEN]);
bool od_fr_marker_decode(const uint8_t in[OD_FR_OS2R_LEN], struct od_fr_marker *m);

enum od_fr_result {
    OD_FR_NONE = 0,     /* no marker: nothing to do */
    OD_FR_REBOOT,       /* done (R8), or a retry after a failed key erase: reboot now */
    OD_FR_LOCKED        /* stage 4: boot on LOCKED, the radio runs, gated commands get FE */
};

/* Is a valid marker stored? (boot step 3) */
bool od_fr_pending(struct od_fr_marker *out);

/* Start a factory reset (writes R1) and run it. */
enum od_fr_result od_fr_start(void);

/* Resume from a stored marker, or report OD_FR_NONE. */
enum od_fr_result od_fr_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_FACTORY_RESET_H */
