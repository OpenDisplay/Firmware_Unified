#include "od_pipe.h"

#include "od_log.h"
#include "od_sec2.h"
#include "od_xfer.h"
#include "od_xfer_internal.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define OD_PIPE_REFRESH_FULL    0u
#define OD_PIPE_REFRESH_FAST    1u
#define OD_PIPE_REFRESH_PARTIAL 2u

OD_STATIC_ASSERT(sizeof(struct PipeStartRequest) == 10u, "PipeStartRequest wire size");
OD_STATIC_ASSERT(sizeof(struct PipePartialExt) == 12u, "PipePartialExt wire size");
OD_STATIC_ASSERT(OD_SEC2_H2D_MIN == 16u, "PIPE protected-frame accounting");
OD_STATIC_ASSERT(PIPE_FRAME_OVERHEAD + 1u + OD_SEC2_H2D_MIN == 20u,
                 "smallest protected PIPE DATA frame");

#if OD_CAP_PIPE

typedef struct {
    bool occupied;
    uint8_t seq;
    uint16_t len;
    uint8_t data[OD_PIPE_REORDER_PAYLOAD];
} od_pipe_reorder_slot_t;

typedef struct {
    bool open;
    bool partial;
    bool gap_open;
    uint8_t window;
    uint8_t ack_every;
    uint16_t frame_eff;
    uint8_t expected_seq;
    bool has_received;
    uint8_t highest_seen;
    uint32_t received_count;
    uint8_t frames_since_ack;
    uint8_t ooo_acks_since_gap;
    uint16_t queued_count;
} od_pipe_state_t;

static od_pipe_state_t s_pipe;
static od_pipe_reorder_slot_t s_reorder[OD_PIPE_REORDER_SLOTS];

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
static struct {
    uint16_t sacks;
    uint16_t reordered;
    uint16_t duplicates;
    uint8_t max_queued;
} s_pipe_log;

static void increment_saturated(uint16_t *value)
{
    if (*value < UINT16_MAX) {
        ++*value;
    }
}
#endif

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
        | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8)
        | (uint32_t)p[3];
}

static uint8_t pipe_slot(uint8_t seq)
{
    return (uint8_t)(seq % OD_PIPE_REORDER_SLOTS);
}

void od_pipe_reset_state(void)
{
    memset(&s_pipe, 0, sizeof s_pipe);
    memset(s_reorder, 0, sizeof s_reorder);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    memset(&s_pipe_log, 0, sizeof s_pipe_log);
#endif
}

size_t od_pipe_log_suffix(char *buf, size_t size)
{
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    int written;

    if (buf == NULL || size == 0u) {
        return 0u;
    }
    buf[0] = '\0';
    if (!s_pipe.open) {
        return 0u;
    }
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    if (od_xfer_pipe_complete() && s_pipe.queued_count == 0u) {
        const uint16_t frames = s_pipe.received_count > UINT16_MAX
            ? UINT16_MAX : (uint16_t)s_pipe.received_count;
        written = snprintf(buf, size, " p[f=%u a=%u r=%u d=%u q=%u]",
                           (unsigned)frames, (unsigned)s_pipe_log.sacks,
                           (unsigned)s_pipe_log.reordered, (unsigned)s_pipe_log.duplicates,
                           (unsigned)s_pipe_log.max_queued);
    } else
#endif
    {
        const uint8_t highest = s_pipe.has_received
            ? s_pipe.highest_seen : (uint8_t)(s_pipe.expected_seq - 1u);
        written = snprintf(buf, size, " p[e=%u h=%u q=%u w=%u]",
                           (unsigned)s_pipe.expected_seq, (unsigned)highest,
                           (unsigned)s_pipe.queued_count, (unsigned)s_pipe.window);
    }
    if (written < 0 || (size_t)written >= size) {
        buf[0] = '\0';
        return 0u;
    }
    return (size_t)written;
#else
    if (buf != NULL && size != 0u) {
        buf[0] = '\0';
    }
    return 0u;
#endif
}

static bool pipe_chunk_received(uint8_t seq)
{
    const uint8_t below = (uint8_t)(s_pipe.expected_seq - 1u - seq);
    const uint32_t accepted = s_pipe.received_count < PIPE_ACK_MASK_BITS
        ? s_pipe.received_count : PIPE_ACK_MASK_BITS;
    const od_pipe_reorder_slot_t *slot;

    if (below < accepted) {
        return true;
    }
    slot = &s_reorder[pipe_slot(seq)];
    return slot->occupied && slot->seq == seq;
}

static void pipe_build_sack(uint8_t out[5])
{
    const uint8_t highest = s_pipe.has_received
        ? s_pipe.highest_seen : (uint8_t)(s_pipe.expected_seq - 1u);
    uint32_t mask = 0u;
    uint8_t i;

    for (i = 0u; i < PIPE_ACK_MASK_BITS; ++i) {
        if (pipe_chunk_received((uint8_t)(highest - 1u - i))) {
            mask |= (uint32_t)1u << i;
        }
    }
    out[0] = highest;
    out[1] = (uint8_t)(mask & 0xffu);
    out[2] = (uint8_t)((mask >> 8) & 0xffu);
    out[3] = (uint8_t)((mask >> 16) & 0xffu);
    out[4] = (uint8_t)((mask >> 24) & 0xffu);
}

static void pipe_update_highest(uint8_t seq)
{
    uint8_t fwd;

    if (!s_pipe.has_received) {
        s_pipe.has_received = true;
        s_pipe.highest_seen = seq;
        return;
    }
    fwd = (uint8_t)(seq - s_pipe.highest_seen);
    if (fwd != 0u && fwd <= PIPE_ACK_MASK_BITS) {
        s_pipe.highest_seen = seq;
    }
}

static od_txq_status_t pipe_send_sack(const od_cmd_ctx_t *ctx)
{
    uint8_t frame[7] = { RESP_ACK, 0x81u, 0u, 0u, 0u, 0u, 0u };
    od_txq_status_t rc;

    pipe_build_sack(frame + 2u);
    rc = od_xfer_reply_app(ctx, frame, (uint16_t)sizeof frame);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    if (rc == OD_TXQ_OK) {
        increment_saturated(&s_pipe_log.sacks);
    }
#endif
    s_pipe.frames_since_ack = 0u;
    s_pipe.ooo_acks_since_gap = 0u;
    return rc;
}

static bool pipe_sack_or_fatal(const od_cmd_ctx_t *ctx)
{
    if (pipe_send_sack(ctx) == OD_TXQ_OK) {
        return true;
    }
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    {
        od_xfer_terminal_snapshot_t snapshot;
        od_xfer_terminal_capture(&snapshot);
        od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_REPLY_DELIVERY,
                                 "DATA", -1, 0u, 0u);
    }
#endif
    od_xfer_pipe_enter_fatal();
    return false;
}

static void pipe_send_start_nack(const od_cmd_ctx_t *ctx, uint8_t error)
{
    const uint8_t frame[] = { RESP_NACK, 0x80u, error, 0u };
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
}

static void pipe_send_data_nack(const od_cmd_ctx_t *ctx, uint8_t error,
                                od_xfer_terminal_cause_t cause)
{
    uint8_t frame[8] = { RESP_NACK, 0x81u, error, 0u, 0u, 0u, 0u, 0u };

    pipe_build_sack(frame + 3u);
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    {
        od_xfer_terminal_snapshot_t snapshot;
        od_xfer_terminal_capture(&snapshot);
        od_xfer_terminal_failure(&snapshot, cause, "DATA", error, 0u, 0u);
    }
#else
    (void)cause;
#endif
    od_xfer_pipe_enter_fatal();
}

static void pipe_send_end_nack(const od_cmd_ctx_t *ctx)
{
    const uint8_t frame[] = { RESP_NACK, 0x82u };
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
}

static od_cmd_result_t pipe_refuse_origin(const od_cmd_ctx_t *ctx, uint8_t opcode,
                                          uint8_t error)
{
    const uint8_t frame[] = { RESP_NACK, opcode, error, 0u };

    od_xfer_log_owner_mismatch((uint16_t)opcode);
    if (od_xfer_reply_app(ctx, frame, (uint16_t)sizeof frame) != OD_TXQ_OK) {
        return OD_CMD_NACK;
    }
    return OD_CMD_NACK;
}

static od_cmd_result_t pipe_finish(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_pipe_mark_finalized();

    const uint8_t end_ack[] = { RESP_ACK, 0x82u };
    const uint8_t refresh_ok[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_SUCCESS };
    const uint8_t refresh_timeout[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_TIMEOUT };
    const bool has_etag = body.n >= 5u;
    const uint32_t new_etag = has_etag ? read_be32(body.p + 1u) : 0u;
    uint8_t refresh_mode = s_pipe.partial ? OD_PIPE_REFRESH_PARTIAL : OD_PIPE_REFRESH_FULL;
    bool completed = false;
    od_xfer_terminal_snapshot_t *snapshot_ptr = NULL;
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    od_xfer_terminal_snapshot_t snapshot;

    snapshot_ptr = &snapshot;
#endif

    if (body.n > 0u && body.p[0] == OD_PIPE_REFRESH_FULL) {
        refresh_mode = OD_PIPE_REFRESH_FULL;
    } else if (body.n > 0u && body.p[0] == OD_PIPE_REFRESH_FAST) {
        refresh_mode = OD_PIPE_REFRESH_FAST;
    }
    if (od_xfer_reply_app(ctx, end_ack, (uint16_t)sizeof end_ack) != OD_TXQ_OK) {
        od_xfer_fail_active(OD_XFER_TERM_REPLY_DELIVERY, "END", -1, 0u, 0u,
                            OD_XFER_ABORT_REPLY_FAILED, s_pipe.partial);
        return OD_CMD_NACK;
    }
    if (od_xfer_pipe_before_refresh() != OD_XFER_BARRIER_PROCEED) {
        od_xfer_pipe_barrier_abort();
        return OD_CMD_NACK;
    }
    if (!od_xfer_pipe_refresh(refresh_mode, has_etag, new_etag, &completed,
                              snapshot_ptr)) {
        pipe_send_end_nack(ctx);
        return OD_CMD_NACK;
    }
    if (od_xfer_reply_app(ctx, completed ? refresh_ok : refresh_timeout,
                          (uint16_t)(completed ? sizeof refresh_ok
                                              : sizeof refresh_timeout)) != OD_TXQ_OK) {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
        od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_REPLY_DELIVERY,
                                 "END", -1, 0u, 0u);
#endif
        return OD_CMD_NACK;
    }
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    if (completed) {
        od_xfer_terminal_complete(&snapshot);
    } else {
        od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_REFRESH_INCOMPLETE,
                                 "END", -1, 0u, 0u);
    }
#endif
    return OD_CMD_OK;
}

od_cmd_result_t od_pipe_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t flags;
    uint8_t requested_w;
    uint8_t requested_n;
    uint16_t client_max_frame;
    uint16_t minimum_frame;
    uint32_t total;
    bool compressed;
    bool partial;
    uint8_t error = OD_ERR_PIPE_START_BAD_HEADER;
    od_xfer_start_cause_t cause;
    uint8_t response[8];

    if (ctx == NULL || !od_span_valid(body)) {
        od_xfer_log_start_refused("PIPE", OD_XFER_START_MALFORMED,
                                  OD_ERR_PIPE_START_BAD_HEADER, false);
        pipe_send_start_nack(ctx, OD_ERR_PIPE_START_BAD_HEADER);
        return OD_CMD_NACK;
    }
    if (ctx->rp.origin != OD_ORIGIN_BLE) {
        return pipe_refuse_origin(ctx, 0x80u, OD_ERR_PIPE_START_BAD_HEADER);
    }

    od_xfer_replace_active();
    if (body.n < sizeof(struct PipeStartRequest) || body.p[0] != PIPE_VERSION) {
        od_xfer_log_start_refused("PIPE", OD_XFER_START_MALFORMED,
                                  OD_ERR_PIPE_START_BAD_HEADER, false);
        pipe_send_start_nack(ctx, OD_ERR_PIPE_START_BAD_HEADER);
        return OD_CMD_NACK;
    }
    flags = body.p[1];
    if ((flags & (uint8_t)~(PIPE_FLAG_COMPRESSED | PIPE_FLAG_PARTIAL)) != 0u) {
        od_xfer_log_start_refused("PIPE", OD_XFER_START_UNSUPPORTED_FLAGS,
                                  OD_ERR_PIPE_START_UNKNOWN_FLAG, false);
        pipe_send_start_nack(ctx, OD_ERR_PIPE_START_UNKNOWN_FLAG);
        return OD_CMD_NACK;
    }
    compressed = (flags & PIPE_FLAG_COMPRESSED) != 0u;
    partial = (flags & PIPE_FLAG_PARTIAL) != 0u;
    if (partial && body.n < sizeof(struct PipeStartRequest) + sizeof(struct PipePartialExt)) {
        od_xfer_log_start_refused("PIPE", OD_XFER_START_MALFORMED,
                                  OD_ERR_PIPE_START_BAD_HEADER, false);
        pipe_send_start_nack(ctx, OD_ERR_PIPE_START_BAD_HEADER);
        return OD_CMD_NACK;
    }

    requested_w = body.p[2];
    requested_n = body.p[3];
    client_max_frame = read_le16(body.p + 4u);
    total = read_le32(body.p + 6u);
    minimum_frame = (uint16_t)(PIPE_FRAME_OVERHEAD + 1u
        + (ctx->was_protected ? OD_SEC2_H2D_MIN : 0u));
    if (client_max_frame < minimum_frame) {
        od_xfer_log_start_refused("PIPE", OD_XFER_START_MALFORMED,
                                  OD_ERR_PIPE_START_BAD_HEADER, false);
        pipe_send_start_nack(ctx, OD_ERR_PIPE_START_BAD_HEADER);
        return OD_CMD_NACK;
    }

#if OD_CAP_PARTIAL
    if (partial) {
        const uint8_t *ext = body.p + sizeof(struct PipeStartRequest);
        cause = od_xfer_pipe_arm_partial(ctx, total, compressed, read_le32(ext),
                                         read_le16(ext + 4u), read_le16(ext + 6u),
                                         read_le16(ext + 8u), read_le16(ext + 10u), &error);
        if (cause != OD_XFER_START_OK) {
            od_xfer_log_start_refused("PIPE", cause, error,
                                      cause == OD_XFER_START_PANEL_GEOMETRY
                                      || cause == OD_XFER_START_SPLIT_LAYOUT);
            pipe_send_start_nack(ctx, error);
            return OD_CMD_NACK;
        }
    } else
#else
    if (partial) {
        od_xfer_log_start_refused("PIPE", OD_XFER_START_PARTIAL_UNSUPPORTED,
                                  OD_ERR_PIPE_START_PARTIAL_UNSUPPORTED, false);
        pipe_send_start_nack(ctx, OD_ERR_PIPE_START_PARTIAL_UNSUPPORTED);
        return OD_CMD_NACK;
    } else
#endif
    cause = od_xfer_pipe_arm_full(ctx, total, compressed);
    if (cause != OD_XFER_START_OK) {
        error = cause == OD_XFER_START_NEEDS_MIGRATION ? OD_ERR_NEEDS_MIGRATION
                                                       : OD_ERR_PIPE_START_SIZE_MISMATCH;
        od_xfer_log_start_refused("PIPE", cause, error,
                                  cause == OD_XFER_START_PANEL_GEOMETRY
                                  || cause == OD_XFER_START_SPLIT_LAYOUT);
        pipe_send_start_nack(ctx, error);
        return OD_CMD_NACK;
    }

    s_pipe.open = true;
    s_pipe.partial = partial;
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    memset(&s_pipe_log, 0, sizeof s_pipe_log);
#endif
    s_pipe.window = requested_w > OD_PIPE_MAX_W ? OD_PIPE_MAX_W : requested_w;
    if (s_pipe.window == 0u) {
        s_pipe.window = 1u;
    }
    s_pipe.ack_every = requested_n > OD_PIPE_MAX_N ? OD_PIPE_MAX_N : requested_n;
    if (s_pipe.ack_every == 0u) {
        s_pipe.ack_every = 1u;
    }
    if (s_pipe.ack_every > s_pipe.window) {
        s_pipe.ack_every = s_pipe.window;
    }
    s_pipe.frame_eff = client_max_frame < PIPE_MAX_FRAME ? client_max_frame : PIPE_MAX_FRAME;

    response[0] = RESP_ACK;
    response[1] = 0x80u;
    response[2] = PIPE_VERSION;
    response[3] = OD_PIPE_MAX_W;
    response[4] = OD_PIPE_MAX_N;
    response[5] = (uint8_t)(PIPE_MAX_FRAME & 0xffu);
    response[6] = (uint8_t)((PIPE_MAX_FRAME >> 8) & 0xffu);
    response[7] = (uint8_t)(0x01u | (partial ? PIPE_FLAG_PARTIAL : 0u));
    if (od_xfer_reply_app(ctx, response, (uint16_t)sizeof response) != OD_TXQ_OK) {
        od_xfer_fail_active(OD_XFER_TERM_REPLY_DELIVERY, "START", -1, 0u, 0u,
                            OD_XFER_ABORT_REPLY_FAILED, partial);
        return OD_CMD_NACK;
    }
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    od_log_debug("PIPE started: %s, window=%u, ack every=%u, frame=%u B",
                 partial ? "partial" : "full", (unsigned)s_pipe.window,
                 (unsigned)s_pipe.ack_every, (unsigned)s_pipe.frame_eff);
#endif
    (void)od_xfer_pipe_activate();
    return OD_CMD_OK;
}

od_cmd_result_t od_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_state_t *xfer = od_xfer_state();
    od_span_t payload;
    uint16_t protection_overhead;
    uint16_t payload_limit;
    uint8_t seq;
    uint8_t fwd;
    uint8_t back;
    od_xfer_stream_result_t consume_result;

    if (ctx == NULL || !od_span_valid(body)) {
        return OD_CMD_NACK;
    }
    if (ctx->rp.origin != OD_ORIGIN_BLE) {
        return pipe_refuse_origin(ctx, 0x81u, 0u);
    }
    if (!s_pipe.open || xfer->mode == OD_XFER_FATAL || body.n < 1u
        || (xfer->mode != OD_XFER_PIPE_FULL && xfer->mode != OD_XFER_PIPE_PARTIAL)) {
        return OD_CMD_NACK;
    }
    if (!od_xfer_owner_matches(ctx)) {
        od_xfer_log_owner_mismatch(CMD_PIPE_WRITE_DATA);
        return OD_CMD_NACK;
    }
    payload = od_span_drop(body, 1u);
    if (ctx->wire_len < PIPE_FRAME_OVERHEAD) {
        pipe_send_data_nack(ctx, 0x03u, OD_XFER_TERM_FRAME_SIZE);
        return OD_CMD_NACK;
    }
    protection_overhead = ctx->was_protected ? OD_SEC2_H2D_MIN : 0u;
    if (ctx->wire_len > s_pipe.frame_eff
        || s_pipe.frame_eff < PIPE_FRAME_OVERHEAD + protection_overhead) {
        pipe_send_data_nack(ctx, 0x03u, OD_XFER_TERM_FRAME_SIZE);
        return OD_CMD_NACK;
    }
    payload_limit = (uint16_t)(s_pipe.frame_eff - PIPE_FRAME_OVERHEAD - protection_overhead);
    if (payload.n > payload_limit || payload.n > OD_PIPE_REORDER_PAYLOAD) {
        pipe_send_data_nack(ctx, 0x03u, OD_XFER_TERM_FRAME_SIZE);
        return OD_CMD_NACK;
    }

    seq = body.p[0];
    fwd = (uint8_t)(seq - s_pipe.expected_seq);
    back = (uint8_t)(s_pipe.expected_seq - seq);
    if (fwd == 0u) {
        if (payload.n > 0u) {
            consume_result = od_xfer_pipe_consume(payload);
            if (consume_result != OD_XFER_STREAM_OK) {
                pipe_send_data_nack(ctx, xfer->compressed ? 0x02u : 0x03u,
                                    od_xfer_stream_cause(consume_result));
                return OD_CMD_NACK;
            }
        }
        s_pipe.expected_seq++;
        s_pipe.received_count++;
        s_pipe.frames_since_ack++;
        pipe_update_highest(seq);
        while (s_reorder[pipe_slot(s_pipe.expected_seq)].occupied
               && s_reorder[pipe_slot(s_pipe.expected_seq)].seq == s_pipe.expected_seq) {
            od_pipe_reorder_slot_t *slot = &s_reorder[pipe_slot(s_pipe.expected_seq)];
            if (slot->len > 0u) {
                consume_result = od_xfer_pipe_consume(od_span_make(slot->data, slot->len));
                if (consume_result != OD_XFER_STREAM_OK) {
                    pipe_send_data_nack(ctx, xfer->compressed ? 0x02u : 0x03u,
                                        od_xfer_stream_cause(consume_result));
                    return OD_CMD_NACK;
                }
            }
            slot->occupied = false;
            if (s_pipe.queued_count > 0u) {
                s_pipe.queued_count--;
            }
            s_pipe.expected_seq++;
            s_pipe.received_count++;
            if (s_pipe.frames_since_ack < UINT8_MAX) {
                s_pipe.frames_since_ack++;
            }
        }
        if (s_pipe.queued_count == 0u) {
            s_pipe.gap_open = false;
        }
        if (!s_pipe.partial && !xfer->compressed && od_xfer_pipe_complete()) {
            if (!pipe_sack_or_fatal(ctx)) {
                return OD_CMD_NACK;
            }
            return pipe_finish(ctx, od_span_none());
        }
        if (s_pipe.frames_since_ack >= s_pipe.ack_every && !pipe_sack_or_fatal(ctx)) {
            return OD_CMD_NACK;
        }
        return OD_CMD_OK;
    }

    if (fwd < s_pipe.window) {
        od_pipe_reorder_slot_t *slot = &s_reorder[pipe_slot(seq)];
        const bool duplicate = slot->occupied && slot->seq == seq;

        if (payload.n > OD_PIPE_REORDER_PAYLOAD) {
            pipe_send_data_nack(ctx, 0x03u, OD_XFER_TERM_FRAME_SIZE);
            return OD_CMD_NACK;
        }
        slot->occupied = true;
        slot->seq = seq;
        slot->len = (uint16_t)payload.n;
        if (payload.n > 0u) {
            memcpy(slot->data, payload.p, payload.n);
        }
        if (!duplicate) {
            s_pipe.queued_count++;
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
            increment_saturated(&s_pipe_log.reordered);
            if (s_pipe.queued_count > s_pipe_log.max_queued) {
                s_pipe_log.max_queued = s_pipe.queued_count > UINT8_MAX
                    ? UINT8_MAX : (uint8_t)s_pipe.queued_count;
            }
#endif
        } else {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
            increment_saturated(&s_pipe_log.duplicates);
#endif
        }
        if (s_pipe.queued_count >= OD_PIPE_REORDER_SLOTS) {
            pipe_send_data_nack(ctx, 0x03u, OD_XFER_TERM_REORDER_FULL);
            return OD_CMD_NACK;
        }
        pipe_update_highest(seq);
        if (!s_pipe.gap_open) {
            s_pipe.gap_open = true;
            if (!pipe_sack_or_fatal(ctx)) {
                return OD_CMD_NACK;
            }
        } else if (++s_pipe.ooo_acks_since_gap >= s_pipe.ack_every
                   && !pipe_sack_or_fatal(ctx)) {
            return OD_CMD_NACK;
        }
        return OD_CMD_OK;
    }

    if (back <= s_pipe.window) {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
        increment_saturated(&s_pipe_log.duplicates);
#endif
        if (!s_pipe.gap_open) {
            if (!pipe_sack_or_fatal(ctx)) {
                return OD_CMD_NACK;
            }
        } else if (++s_pipe.ooo_acks_since_gap >= s_pipe.ack_every
                   && !pipe_sack_or_fatal(ctx)) {
            return OD_CMD_NACK;
        }
        return OD_CMD_OK;
    }

    pipe_send_data_nack(ctx, 0x04u, OD_XFER_TERM_SEQUENCE_WINDOW);
    return OD_CMD_NACK;
}

od_cmd_result_t od_pipe_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_state_t *xfer = od_xfer_state();
    od_xfer_stream_result_t finalize_result;

    if (ctx == NULL || !od_span_valid(body)) {
        return OD_CMD_NACK;
    }
    if (ctx->rp.origin != OD_ORIGIN_BLE) {
        return pipe_refuse_origin(ctx, 0x82u, 0u);
    }
    if (!s_pipe.open) {
        pipe_send_end_nack(ctx);
        return OD_CMD_NACK;
    }
    if (!od_xfer_owner_matches(ctx)) {
        od_xfer_log_owner_mismatch(CMD_PIPE_WRITE_END);
        return OD_CMD_NACK;
    }
    if (xfer->mode == OD_XFER_FATAL) {
        pipe_send_end_nack(ctx);
        od_xfer_abort_active(OD_XFER_ABORT_PIPE_INCOMPLETE, s_pipe.partial);
        return OD_CMD_NACK;
    }
    if (!pipe_sack_or_fatal(ctx)) {
        return OD_CMD_NACK;
    }
    finalize_result = s_pipe.queued_count == 0u
        ? od_xfer_pipe_finalize() : OD_XFER_STREAM_OK;
    if (s_pipe.queued_count != 0u || finalize_result != OD_XFER_STREAM_OK
        || !od_xfer_pipe_complete()) {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
        {
            od_xfer_terminal_snapshot_t snapshot;
            const od_xfer_terminal_cause_t cause = finalize_result != OD_XFER_STREAM_OK
                ? od_xfer_stream_cause(finalize_result) : OD_XFER_TERM_INCOMPLETE;
            od_xfer_terminal_capture(&snapshot);
            od_xfer_terminal_failure(&snapshot, cause, "END", -1, 0u, 0u);
        }
#endif
        pipe_send_end_nack(ctx);
        od_xfer_abort_active(OD_XFER_ABORT_PIPE_INCOMPLETE, s_pipe.partial);
        return OD_CMD_NACK;
    }
    return pipe_finish(ctx, body);
}

#else

void od_pipe_reset_state(void) { }

size_t od_pipe_log_suffix(char *buf, size_t size)
{
    if (buf != NULL && size != 0u) {
        buf[0] = '\0';
    }
    return 0u;
}

od_cmd_result_t od_pipe_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const uint8_t frame[] = { RESP_NACK, 0x80u, 0x04u, 0u };
    (void)body;
    od_xfer_reply_error(ctx, frame, (uint16_t)sizeof frame);
    return OD_CMD_NACK;
}

od_cmd_result_t od_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx;
    (void)body;
    return OD_CMD_UNKNOWN;
}

od_cmd_result_t od_pipe_end(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx;
    (void)body;
    return OD_CMD_UNKNOWN;
}

#endif
