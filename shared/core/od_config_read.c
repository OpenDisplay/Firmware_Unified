/* od_config_read.c -- see od_config_read.h for why this is a producer and not a loop. */

#include "od_config_read.h"

#include "od_cfg_policy.h"
#include "od_reply.h"

#include <string.h>

/* The chunk frame, byte for byte as the authority target emits it (Firmware
 * communication.cpp:565-577): [ACK][RESP_CONFIG_READ][chunk_lo][chunk_hi], then on chunk 0 only
 * [len_lo][len_hi], then data. Changing any of it is a wire change, so it is reproduced rather
 * than tidied -- in particular the total length is little-endian and appears ONCE. */
#define HDR_COMMON 4u
#define HDR_FIRST  (HDR_COMMON + 2u)

static struct {
    bool                active;
    od_reply_t          rp;
    od_tx_reservation_t res;         /* transferred in by start(); pays for chunk 0 */
    const uint8_t      *blob;
    uint32_t            blob_len;
    uint32_t            offset;
    uint16_t            chunk;
} s;

/* Build and queue one chunk from the current offset. Spends one unit of `r`. */
static od_txq_status_t emit_chunk(od_tx_reservation_t *r)
{
    uint8_t frame[MAX_RESPONSE_DATA_SIZE];
    uint16_t n = 0u;
    uint16_t room;
    uint32_t remaining = s.blob_len - s.offset;
    uint16_t take;

    frame[n++] = RESP_ACK;
    frame[n++] = RESP_CONFIG_READ;
    frame[n++] = (uint8_t)(s.chunk & 0xFFu);
    frame[n++] = (uint8_t)((s.chunk >> 8) & 0xFFu);
    if (s.chunk == 0u) {
        frame[n++] = (uint8_t)(s.blob_len & 0xFFu);
        frame[n++] = (uint8_t)((s.blob_len >> 8) & 0xFFu);
    }

    room = (uint16_t)(MAX_RESPONSE_DATA_SIZE - n);
    take = (remaining < (uint32_t)room) ? (uint16_t)remaining : room;
    if (take > 0u) {
        memcpy(frame + n, s.blob + s.offset, take);
        od_cfg_policy_read_view(s.blob, s.blob_len, s.offset, frame + n, take);   /* 10.2 */
        n = (uint16_t)(n + take);
    }

    {
        const od_txq_status_t rc = od_reply(r, &s.rp, frame, n);
        memset(frame, 0, sizeof frame);   /* a claimed read view carries K_dev */
        if (rc != OD_TXQ_OK) {
            return rc;               /* the chunk is NOT advanced; the pass retries it */
        }
    }
    s.offset += take;
    s.chunk++;
    if (s.offset >= s.blob_len) {
        s.active = false;            /* the last chunk is queued; the read is done */
    }
    return OD_TXQ_OK;
}

od_txq_status_t od_config_read_start(const od_reply_t *rp, od_tx_reservation_t *r,
                                     const uint8_t *blob, uint32_t blob_len)
{
    if (rp == NULL || r == NULL) {
        return OD_TXQ_INVARIANT;
    }
    if (s.active) {
        /* The dispatcher is supposed to have deferred this. Refusing rather than restarting keeps
         * the first read's promised chunk count honest -- a host told "12 chunks" and then handed
         * a different config's chunk 3 has no way to detect the splice. */
        return OD_TXQ_INVARIANT;
    }

    memset(&s, 0, sizeof s);
    s.rp = *rp;                      /* by value: the dispatcher's frame context does not persist */

    if (blob == NULL || blob_len == 0u) {
        /* A load failure is a COMPLETED read with one error frame, not a pending producer. */
        uint8_t err[4];
        err[0] = RESP_NACK;
        err[1] = RESP_CONFIG_READ;
        err[2] = 0x00u;
        err[3] = 0x00u;
        /* od_reply_PLAIN: this is a hard NACK, and section 3.6 puts every one of those on the
         * explicit plaintext path. Sealing it would make a read failure unreadable to a client
         * whose session had just died -- the same class as the Nordic regression that sealed its
         * own rejection frames. */
        return od_reply_plain(r, rp, err, sizeof err);
    }

    s.blob = blob;
    s.blob_len = blob_len;
    s.active = true;
    s.res = *r;
    r->remaining = 0u;               /* the token is TRANSFERRED; the caller must not reuse it */

    {
        const od_txq_status_t rc = emit_chunk(&s.res);
        if (rc != OD_TXQ_OK) {
            /* Chunk 0 could not be queued, so nothing was promised to the host and there is no
             * partial read to preserve. */
            od_txq_release(&s.res);
            s.active = false;
        }
        return rc;
    }
}

od_txq_status_t od_config_read_pump(void)
{
    od_tx_reservation_t r;
    od_txq_status_t rc;

    if (!s.active) {
        return OD_TXQ_OK;
    }
    /* One slot per pass. FULL is the normal backpressure answer and leaves the producer pending --
     * it is not an error and must not produce a frame, because a frame would need a slot too. */
    rc = od_txq_reserve(1u, &r);
    if (rc != OD_TXQ_OK) {
        return rc;
    }
    rc = emit_chunk(&r);
    od_txq_release(&r);              /* returns the unit if emit_chunk did not spend it */

    /* ONLY FULL IS RETRYABLE, and treating everything else as retryable is how a read becomes
     * permanent. A session that expires mid-read makes od_reply() substitute a plaintext NACK and
     * report SEAL_FAILED; leaving the producer active then re-emits the same chunk every pass,
     * queues another NACK every pass, and -- because od_dispatch DEFERS while a read is active --
     * refuses every later config read and write for the life of the boot.
     *
     * The read is over either way. GONE means the peer is gone, SEAL_FAILED and TOO_LARGE will
     * fail identically on the next attempt, and INVARIANT is a bug. The host has already been
     * answered: od_reply substituted a NACK on the failures that produce one, and on the rest it
     * simply never receives the remaining chunks and times out -- which is the honest outcome for
     * a transfer that cannot continue. */
    if (rc != OD_TXQ_OK && rc != OD_TXQ_FULL) {
        od_config_read_cancel();
    }
    return rc;
}

bool od_config_read_active(void)
{
    return s.active;
}

void od_config_read_cancel(void)
{
    if (!s.active) {
        return;
    }
    /* Normally a no-op: the transferred unit is spent on chunk 0 and every later chunk uses a
     * fresh per-pass reservation, so there is no unused capacity to give back. Kept because it
     * costs nothing and makes the invariant total -- if start() ever holds units across passes,
     * this is already correct. */
    od_txq_release(&s.res);
    s.active = false;
    s.blob = NULL;
}
