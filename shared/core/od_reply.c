/* od_reply.c -- see od_reply.h for why confidentiality is chosen by the call, not by the bytes. */

#include "od_reply.h"

#include "od_log.h"
#include "od_sec2.h"

#include <string.h>

#if OD_CAP_LOG
static void log_queue_result(od_txq_status_t rc, const uint8_t *frame, uint16_t len)
{
    const uint16_t cmd = (frame != NULL && len >= 2u)
        ? (uint16_t)(((uint16_t)frame[0] << 8) | frame[1]) : 0u;

    if (rc == OD_TXQ_GONE) {
        od_log_info("Response was not queued because the reply target is gone (cmd=0x%04X)",
                    (unsigned)cmd);
    } else if (rc != OD_TXQ_OK) {
        od_log_error("Response could not be queued (cmd=0x%04X, rc=%d)",
                     (unsigned)cmd, (int)rc);
    }
}

static void log_nack_queue_result(od_txq_status_t rc, uint8_t cmd_low)
{
    if (rc == OD_TXQ_GONE) {
        od_log_info("Response NACK was not queued because the reply target is gone (cmd=0x%02X)",
                    (unsigned)cmd_low);
    } else if (rc != OD_TXQ_OK) {
        od_log_error("Response NACK could not be queued (cmd=0x%02X, rc=%d)",
                     (unsigned)cmd_low, (int)rc);
    }
}
#else
#define log_queue_result(rc_, frame_, len_) \
    ((void)(rc_), (void)(frame_), (void)(len_))
#define log_nack_queue_result(rc_, cmd_) ((void)(rc_), (void)(cmd_))
#endif

static bool s_plain_scope;

static od_txq_status_t queue_response(od_tx_reservation_t *r, const od_reply_t *rp,
                                      const uint8_t *frame, uint16_t len)
{
    const od_txq_status_t rc = od_txq_commit(r, rp, frame, len);

    log_queue_result(rc, frame, len);
    return rc;
}

/* The one substitution this file makes. When a response cannot be sealed, sending it in the clear
 * would hand the host payload it expected protected, and sending nothing would hang a client
 * waiting on an ack. A three-byte hard NACK does neither: {0xFF, cmd, 0x00} echoes the opcode so
 * the client can match it to its request, and 0xFF at byte 0 is what every target already routes
 * plaintext. */
static od_txq_status_t queue_hard_nack(od_tx_reservation_t *r, const od_reply_t *rp,
                                       uint8_t cmd_low, od_txq_status_t why)
{
    uint8_t nack[3];
    od_txq_status_t rc;

    nack[0] = RESP_NACK;
    nack[1] = cmd_low;
    nack[2] = 0x00u;
    rc = od_txq_commit(r, rp, nack, sizeof nack);
    log_nack_queue_result(rc, cmd_low);
    /* The substitution's own failure wins: the caller needs to know nothing was queued at all,
     * which is a different situation from "your response became a NACK". */
    return (rc == OD_TXQ_OK) ? why : rc;
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    return queue_response(r, rp, frame, len);
}

void od_reply_plain_scope(bool on)
{
    s_plain_scope = on;
}

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    uint8_t sealed[OD_SEC2_SEALED_MAX];
    uint16_t sealed_len = 0u;
    enum od_sec2_seal rc;

    if (r == NULL || rp == NULL || frame == NULL || len == 0u) {
        od_log_error("Application response arguments are invalid "
                     "(reservation=%u, target=%u, frame=%u, len=%u)",
                     r != NULL ? 1u : 0u, rp != NULL ? 1u : 0u,
                     frame != NULL ? 1u : 0u, (unsigned)len);
        return OD_TXQ_INVARIANT;
    }
    /* Sealing applies to BLE on a claimed (or LOCKED) device, outside the plaintext set. TLS
     * already protects LAN-TLS, and a claimed device answers LAN-plain only with plaintext-set
     * frames and FE. */
    if (rp->origin != OD_ORIGIN_BLE || s_plain_scope || od_sec2_state() == OD_SEC2_UNCLAIMED) {
        return queue_response(r, rp, frame, len);
    }
    if (len < 2u) {
        return OD_TXQ_INVARIANT;          /* no opcode to echo in a NACK */
    }
    if (!od_sec2_session_live(rp)) {
        /* Meant to be protected and there is no session: never in the clear. */
        return queue_hard_nack(r, rp, frame[1], OD_TXQ_SEAL_FAILED);
    }

    rc = od_sec2_seal(od_span_make(frame, len), sealed, sizeof sealed, &sealed_len);
    switch (rc) {
    case OD_SEC2_SEAL_OK:
        return queue_response(r, rp, sealed, sealed_len);
    case OD_SEC2_SEAL_TOO_LONG:
    case OD_SEC2_SEAL_NO_ROOM:
        return queue_hard_nack(r, rp, frame[1], OD_TXQ_TOO_LARGE);
    case OD_SEC2_SEAL_NO_SESSION:
    case OD_SEC2_SEAL_CRYPTO_ERROR:   /* the counter value is spent; never resealed */
    case OD_SEC2_SEAL_EXHAUSTED:      /* the session is gone; the client re-runs HELLO */
        return queue_hard_nack(r, rp, frame[1], OD_TXQ_SEAL_FAILED);
    }
    /* No default above: -Wswitch fails the build on a new od_sec2_seal result. Unreachable. */
    return queue_hard_nack(r, rp, frame[1], OD_TXQ_SEAL_FAILED);
}
