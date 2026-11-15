/* od_gate.h -- the SEC2 gating table (docs/SECURITY_V2.md section 6) for every gated opcode.
 *
 * The dispatcher answers the plaintext set itself (0x0043, 0x0044, 0x0046, 0x0050, 0x0054-0x0056)
 * before it gets here. Everything else meets od_gate_admit(), which decides, per device state,
 * transport and claim mode, whether the frame runs and with which body:
 *
 *   LOCKED                         FE, every origin
 *   UNCLAIMED, BLE or LAN          plaintext; in claim mode FD unless allow-listed
 *   CLAIMED, LAN TLS               plaintext (TLS protects it; K_lan is the PSK)
 *   CLAIMED, LAN plain             FE
 *   CLAIMED, BLE, no live session  FE
 *   CLAIMED, BLE, live session     the frame must open as a record (section 9.5), then claim mode
 *                                  refusals apply, sealed
 *
 * EXHAUSTIVENESS IS ENFORCED BY THE COMPILER: the switch over od_sec2_open carries no `default`,
 * so -Wswitch (with -Werror) fails the build on a new member without a disposition here.
 *
 * CONTEXT: single loop/main task. No locks.
 */

#ifndef OD_GATE_H
#define OD_GATE_H

#include "od_cmd.h"
#include "od_span.h"
#include "od_txq.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    od_frame_outcome_t outcome;
    od_span_t          body;     /* valid ONLY when outcome == ACCEPTED */
    bool               sealed;   /* the body came out of a verified record */
} od_gate_result_t;

/* The claim-mode allow-list (section 6): may `cmd` run while claim mode is active? `claimed` is
 * the claiming connection's sealed frame after the claim (section 7.3: config writes are allowed
 * in that session, secrets included; every other CM refusal still applies). */
bool od_gate_claim_mode_allows(uint16_t cmd, bool fresh, bool config_locked, bool claimed);

/* No response is ever sent for these, so record-layer refusals are silent too (section 9.5
 * "no-ack frames"). */
bool od_gate_no_ack(uint16_t cmd);

/* Admit or refuse one gated frame. `body` is the bytes after the opcode. `scratch` receives the
 * opened record's payload and must hold OD_SEC2_PLAIN_MAX; the returned body points into it.
 * On any non-ACCEPTED outcome the refusal has been queued (or deliberately withheld) and the
 * caller emits nothing more. */
od_gate_result_t od_gate_admit(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                               od_span_t body, uint8_t *scratch, size_t scratch_cap);

#ifdef __cplusplus
}
#endif

#endif /* OD_GATE_H */
