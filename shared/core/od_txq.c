/* od_txq.c -- see od_txq.h for why capacity is a counter with a token rather than reserved slots. */

#include "od_txq.h"

#include "od_log.h"

#include <string.h>

struct od_txq_entry {
    od_origin_t origin;
    uint32_t    tag;
    uint16_t    len;
    uint8_t     data[OD_TX_FRAME_MAX];
};

/* A ring with one slot deliberately left unused, so head == tail means empty and there is no
 * separate count to keep consistent with the indices. Usable capacity is OD_TXQ_SLOTS - 1, which
 * is what the PIPE_MAX_W + 2 sizing rule is stated against. */
static struct od_txq_entry s_ring[OD_TXQ_SLOTS];
static uint16_t s_head;              /* next entry to send */
static uint16_t s_tail;              /* next free slot */
static uint8_t  s_reserved;          /* units claimed by live tokens, not yet spent */
static uint8_t  s_gen;               /* bumped by reset; a token from an older one is dead */

static uint16_t ring_next(uint16_t i)
{
    return (uint16_t)((i + 1u) % OD_TXQ_SLOTS);
}

static uint16_t queued(void)
{
    return (uint16_t)((s_tail + OD_TXQ_SLOTS - s_head) % OD_TXQ_SLOTS);
}

static uint16_t free_slots(void)
{
    return (uint16_t)((OD_TXQ_SLOTS - 1u) - queued());
}

void od_txq_reset(void)
{
    s_head = 0u;
    s_tail = 0u;
    s_reserved = 0u;
    /* Every outstanding token dies here. It cannot be reached to be cleared, so the generation is
     * how it learns: a stale commit would otherwise decrement a count this line just zeroed. */
    ++s_gen;
    memset(s_ring, 0, sizeof s_ring);
}

od_txq_status_t od_txq_reserve(uint8_t count, od_tx_reservation_t *r)
{
    if (r == NULL || count == 0u) {
        return OD_TXQ_INVARIANT;
    }
    r->remaining = 0u;
    /* Everything already promised to other tokens is spoken for, even though those units are not
     * in the ring yet -- that pending claim is the whole point of reserving. The first test is not
     * redundant: written as one subtraction it underflows in unsigned arithmetic and turns a full
     * queue into an unlimited one. */
    if ((uint16_t)s_reserved >= free_slots()) {
        return OD_TXQ_FULL;
    }
    if ((uint16_t)count > free_slots() - (uint16_t)s_reserved) {
        return OD_TXQ_FULL;
    }
    s_reserved = (uint8_t)(s_reserved + count);
    r->remaining = count;
    r->gen = s_gen;
    return OD_TXQ_OK;
}

void od_txq_release(od_tx_reservation_t *r)
{
    if (r == NULL || r->remaining == 0u) {
        return;                       /* idempotent: every path calls this, including early exits */
    }
    if (r->gen == s_gen) {
        s_reserved = (uint8_t)(s_reserved - r->remaining);
    }
    /* A stale token still zeroes itself, so a later release cannot double-count it, but it must
     * not give units back to a queue that never issued them. */
    r->remaining = 0u;
}

od_txq_status_t od_txq_commit(od_tx_reservation_t *r, const od_reply_t *rp,
                              const uint8_t *frame, uint16_t len)
{
    struct od_txq_entry *e;

    if (r == NULL || rp == NULL || frame == NULL || len == 0u) {
        return OD_TXQ_INVARIANT;
    }
    if (r->remaining == 0u) {
        /* The handler emitted more replies than its opcode reserved. Refusing is the honest
         * outcome; borrowing would take capacity another in-flight dispatch is relying on and turn
         * a local bug into a lost ack somewhere else. */
        return OD_TXQ_INVARIANT;
    }
    if (r->gen != s_gen) {
        /* The queue was reset under this dispatch -- a link teardown, od_core_reset(). The reply
         * is for a world that no longer exists, and spending a unit here would decrement a count
         * the reset already zeroed. */
        r->remaining = 0u;
        return OD_TXQ_INVARIANT;
    }
    if (len > OD_TX_FRAME_MAX) {
        return OD_TXQ_TOO_LARGE;
    }
    /* BLE's usable ATT value is 3 bytes below the frame; the entry is wider only so one storage
     * width serves every origin. LAN is bounded by its own transport, not here. */
    if (rp->origin == OD_ORIGIN_BLE && len > OD_TXQ_VALUE_MAX_BLE) {
        return OD_TXQ_TOO_LARGE;
    }
    if (!od_hal_radio_tag_is_live(rp->origin, rp->tag)) {
        /* The unit stays spent. The caller's dispatch is over either way, and its release() will
         * return whatever is left; handing the unit back here would let a doomed reply free
         * capacity it never used and mask the ordering bug that produced it. */
        r->remaining = (uint8_t)(r->remaining - 1u);
        s_reserved = (uint8_t)(s_reserved - 1u);
        return OD_TXQ_GONE;
    }
    if (free_slots() == 0u) {
        /* Unreachable while every commit is covered by a reserve, which is the invariant this
         * module exists to hold. Reported rather than asserted so a violation is visible in the
         * field instead of silently overwriting the oldest entry. */
        return OD_TXQ_INVARIANT;
    }

    e = &s_ring[s_tail];
    e->origin = rp->origin;
    e->tag = rp->tag;
    e->len = len;
    memcpy(e->data, frame, len);
    s_tail = ring_next(s_tail);

    r->remaining = (uint8_t)(r->remaining - 1u);
    s_reserved = (uint8_t)(s_reserved - 1u);
    return OD_TXQ_OK;
}

/* One line per discarded entry, and INFO rather than warn or error on purpose: a normal
 * disconnect mid-upload discards every frame still queued for that link, so a louder level would
 * make a routine event look like a fault. What it exists to make visible is the abnormal case --
 * entries discarded while a link is up, which means a reply's tag stopped matching the live owner
 * and points at an identity bug rather than a departed peer.
 *
 * Deliberately not throttled: unlike the record lines in od_sec2.c a peer cannot drive this at
 * will, and losing one of these hides the cause of a client-side timeout. */
static void log_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    od_log_info("TX dropped: origin=%u tag=%08lX len=%u reason=%d",
                (unsigned)(rp != NULL ? (unsigned)rp->origin : 0u),
                (unsigned long)(rp != NULL ? rp->tag : 0u), (unsigned)len, (int)why);
    od_txq_app_dropped(rp, len, why);
}

/* Tell the target about every entry a tag teardown is about to discard, before they are gone. */
static void report_dropped_tag(od_origin_t origin, uint32_t tag, od_radio_result_t why)
{
    uint16_t i = s_head;

    while (i != s_tail) {
        if (s_ring[i].origin == origin && s_ring[i].tag == tag) {
            const od_reply_t rp = { origin, tag };
            log_dropped(&rp, s_ring[i].len, why);
        }
        i = ring_next(i);
    }
}

/* Drop every queued entry belonging to `tag`, preserving the order of the rest. Called when the
 * radio reports the link is gone: the remaining entries for it are undeliverable, and leaving them
 * would block the head for a connection that no longer exists. */
static void drop_tag(od_origin_t origin, uint32_t tag)
{
    uint16_t read = s_head;
    uint16_t write = s_head;

    while (read != s_tail) {
        if (!(s_ring[read].origin == origin && s_ring[read].tag == tag)) {
            if (write != read) {
                s_ring[write] = s_ring[read];
            }
            write = ring_next(write);
        }
        read = ring_next(read);
    }
    s_tail = write;
}

uint16_t od_txq_process(void)
{
    uint16_t retired = 0u;

    while (s_head != s_tail) {
        struct od_txq_entry *e = &s_ring[s_head];
        od_radio_result_t rc;

        if (!od_hal_radio_tag_is_live(e->origin, e->tag)) {
            report_dropped_tag(e->origin, e->tag, OD_RADIO_GONE);
            drop_tag(e->origin, e->tag);
            continue;                 /* head now names a different entry, or the queue is empty */
        }
        rc = od_hal_radio_send(e->origin, e->tag, e->data, e->len);
        if (rc == OD_RADIO_RETRY) {
            break;                    /* keep the entry; ordering forbids skipping past it */
        }
        if (rc == OD_RADIO_GONE) {
            report_dropped_tag(e->origin, e->tag, OD_RADIO_GONE);
            drop_tag(e->origin, e->tag);
            continue;
        }
        if (rc == OD_RADIO_ERROR) {
            /* Logged, because a dropped response is invisible from the wire: the host just
             * waits. Without this the only symptom is a client timing out for no stated reason. */
            const od_reply_t rp = { e->origin, e->tag };
            log_dropped(&rp, e->len, OD_RADIO_ERROR);
        }
        /* SENT and ERROR both retire this entry: one succeeded, the other can never succeed. */
        s_head = ring_next(s_head);
        ++retired;
    }
    return retired;
}

od_txq_status_t od_txq_flush(uint32_t now_ms, uint32_t deadline_ms)
{
    if (s_head == s_tail) {
        return OD_TXQ_OK;
    }
    /* Wrap-safe "has now reached the deadline": the difference is taken in the unsigned domain and
     * read as signed, which stays correct across the 49.7-day uint32_t rollover provided both
     * stamps come from one clock -- the same rule the od_sec2 session timeout follows. */
    if ((int32_t)(now_ms - deadline_ms) >= 0) {
        return OD_TXQ_TIMEOUT;        /* entries stay queued: late beats dropped */
    }
    (void)od_txq_process();
    /* ONE drain attempt per call, deliberately. This module owns no clock and must not block, so
     * it cannot wait out a deadline itself. The caller re-enters with an advanced now_ms, which
     * keeps the waiting in the loop that also feeds the watchdog.
     *
     * BUSY, NOT TIMEOUT: the deadline has not been reached, so the transport may still recover and
     * the caller must come back. Returning TIMEOUT here would tell a caller reading the status at
     * face value to give up and start the refresh, and the END ack it was waiting for would land
     * after the panel had already begun -- the exact thing the barrier exists to prevent. */
    return (s_head == s_tail) ? OD_TXQ_OK : OD_TXQ_BUSY;
}

uint16_t od_txq_depth(void)
{
    return queued();
}

uint8_t od_txq_reserved(void)
{
    return s_reserved;
}
