/* od_cmd_reply.h -- the reply entry point every Nordic handler calls.
 *
 * A thin named seam over od_reply(), kept because the classification below is a per-call-site
 * decision that has to be legible at the site.
 *
 * THE CLASSIFICATION IS THE POINT, and this target is the reason the rule exists. The sender this replaces
 * inferred confidentiality from the response bytes -- byte 2, with a carve-out for the 7-byte PIPE
 * ACK whose byte 2 is a rolling sequence number. Before that carve-out it read byte 0 and so
 * SEALED its own rejection frames, which py-opendisplay then decrypted and validated as an ACK for
 * a command the device had refused. A response is bytes whose meaning depends on the opcode, so
 * the producer states its intent instead:
 *
 *   od_cmd_reply_plain()    control frames -- FIRMWARE_VERSION and the rest of the plaintext set.
 *                           Never sealed, whatever the session.
 *   od_cmd_reply()          application responses, INCLUDING all PIPE ACKs.
 *   od_cmd_reply_refusal()  a handler's NACK to a gated command. Sealed while a session is live,
 *                           plaintext otherwise (od_reply_refusal, SECURITY_V2 9.5).
 *
 * No handler may choose by looking at its own payload.
 *
 * RETURNS A STATUS, and it is not decorative: after a non-OK the caller MUST NOT emit another wire
 * response. od_reply() can substitute a plaintext hard NACK for a frame it could not seal, and a
 * multi-reply path that ignored that would queue a contradictory success behind it.
 */

#ifndef OD_CMD_REPLY_H
#define OD_CMD_REPLY_H

#include "od_cmd.h"
#include "od_txq.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len);
od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len);
od_txq_status_t od_cmd_reply_refusal(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len);

/* Drain queued replies before a blocking panel refresh. Call it AFTER the END ack is queued and
 * IMMEDIATELY BEFORE the refresh, on every path that blocks.
 *
 * WITHOUT THIS THE ACK DOES NOT LEAVE. od_cmd_reply() only ENQUEUES now; the drain runs in the
 * pump, which cannot re-enter until the handler returns -- and the handler is inside a refresh
 * that can hold the main thread for a minute. The host spends its 500 ms tail-flush read, probes,
 * and aborts a transfer that in fact completed, and a disconnect on the way out can discard the
 * queued ack entirely. The synchronous sender this replaced had the property for free; the queue
 * does not, and that is the one regression a cutover to shared egress introduces for free.
 *
 * BOUNDED: on the deadline od_txq_flush() reports TIMEOUT and LEAVES THE ENTRIES QUEUED -- late
 * beats dropped -- so there is nothing for a caller to decide. Hence void. */
void od_cmd_flush_before_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_REPLY_H */
