/* od_cmd_reply.h -- the reply entry point every ESP32 handler calls.
 *
 * A thin named seam over od_reply(), kept because the classification below is a per-call-site
 * decision that has to be legible at the site, and because the targets differ in how they log.
 *
 * The classification is the point. The shipped code inferred confidentiality from the response
 * bytes, and that inference was the defect: ESP32 read byte 2 with a PIPE-ACK carve-out, and
 * Nordic read byte 0 and so sealed its own rejection frames, which the host then validated as an
 * ACK for a command the device refused. A response is bytes whose meaning depends on the opcode,
 * so the producer states its intent instead:
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
 * IMMEDIATELY BEFORE the refresh, on every path that blocks: a full-image refresh and both partial
 * ones (direct and PIPE).
 *
 * The loop task is the queue's only drainer, and a refresh plus its wait occupies that task for
 * seconds -- up to the ~240 s panel bound the watchdog is sized for. An ack still sitting in the
 * queue when the refresh starts therefore does not reach air until the panel is finished, by which
 * time the host has spent its 500 ms tail-flush read, dup-probed three times and aborted a transfer
 * that in fact completed.
 *
 * BOUNDED, because the peer can vanish mid-refresh and a queue that never drains must not wedge the
 * loop. On the deadline od_txq_flush() reports TIMEOUT and LEAVES THE ENTRIES QUEUED -- they go out
 * after the refresh, late rather than dropped -- so there is nothing here to report and no caller
 * decision to make. Hence void.
 *
 * The bound rests on od_hal_radio.h's MUST NOT BLOCK, which both arms now honour: BLE returns
 * RETRY on backpressure, and the LAN socket carries SO_SNDTIMEO so a stalled peer cannot park a
 * flush. Break that promise in either transport and this deadline stops meaning anything.
 *
 * The deadline is half the host's tail-flush read, so a delayed ack still beats the first PTO probe
 * rather than racing it; at a slow 30 ms connection interval it also covers ~8 notification
 * opportunities against a deepest tail of 3 frames.
 *
 */
void od_cmd_flush_before_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CMD_REPLY_H */
