/* sec2_client_direct.c -- sec2c_deliver() for a suite that does not link od_dispatch.
 *
 * Only the plaintext SEC opcodes are delivered (the client sends nothing else through here), each
 * with the one-unit reservation od_dispatch_frame() would take for it. Compiled into the
 * executable, so the archive's dispatcher-backed definition is never pulled.
 */

#include "sec2_client.h"

#include "od_sec2.h"

od_frame_outcome_t sec2c_deliver(const od_reply_t *rp, od_span_t frame)
{
    od_tx_reservation_t r;
    od_frame_outcome_t out;
    uint16_t cmd;

    if (frame.n < 2u) {
        return OD_FRAME_REJECTED_FRAME;
    }
    cmd = (uint16_t)(((uint16_t)frame.p[0] << 8) | frame.p[1]);
    if (od_txq_reserve(1u, &r) != OD_TXQ_OK) {
        return OD_FRAME_DEFERRED;
    }
    out = od_sec2_command(&r, rp, cmd, od_span_make(frame.p + 2, frame.n - 2u));
    od_txq_release(&r);
    return out;
}
