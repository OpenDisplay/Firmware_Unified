#include "od_xfer_internal.h"

#include "opendisplay_protocol.h"

#include <limits.h>
#include <string.h>

#define OD_REFRESH_FULL 0u
#define OD_REFRESH_FAST 1u

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

od_cmd_result_t od_xfer_direct_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_panel_info_t panel;
    od_xfer_state_t *state = od_xfer_state();
    const bool compressed = body.n >= 4u;
    uint32_t expected;
    const uint8_t ack[] = { RESP_ACK, RESP_DIRECT_WRITE_START_ACK };

    if (ctx == NULL || !od_span_valid(body) || body.n > UINT32_MAX) {
        od_xfer_log_start_refused("Direct write", OD_XFER_START_MALFORMED, -1, false);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_START_ACK);
        return OD_CMD_NACK;
    }

    od_xfer_replace_active();
    od_xfer_app_prepare_start();
    memset(&panel, 0, sizeof panel);
    if (od_xfer_app_panel_info(&panel) && panel.needs_migration) {
        const uint8_t nack[] = { RESP_NACK, RESP_DIRECT_WRITE_START_ACK,
                                 OD_ERR_NEEDS_MIGRATION, 0u };
        od_xfer_log_start_refused("Direct write", OD_XFER_START_NEEDS_MIGRATION,
                                  OD_ERR_NEEDS_MIGRATION, false);
        od_xfer_reply_error(ctx, nack, (uint16_t)sizeof nack);
        return OD_CMD_NACK;
    }
    if (panel.geometry.total_bytes == 0u) {
        od_xfer_log_start_refused("Direct write", OD_XFER_START_PANEL_GEOMETRY, -1, true);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_START_ACK);
        return OD_CMD_NACK;
    }
    if (panel.geometry.layout == OD_COLOR_LAYOUT_SPLIT_HALVES && !split_halves_ok(&panel)) {
        od_xfer_log_start_refused("Direct write", OD_XFER_START_SPLIT_LAYOUT, -1, true);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_START_ACK);
        return OD_CMD_NACK;
    }
    expected = panel.geometry.total_bytes;
    if (compressed && read_le32(body.p) != expected) {
        od_xfer_log_start_refused("Direct write", OD_XFER_START_SIZE_MISMATCH, -1, false);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_START_ACK);
        return OD_CMD_NACK;
    }

    od_xfer_clear_state();
    state->mode = OD_XFER_DIRECT_FULL;
    state->owner = ctx->rp;
    state->started_ms = od_xfer_app_now_ms();
    state->expected_bytes = expected;
    state->compressed = compressed;
    state->geometry = panel.geometry;

    od_xfer_log_start();
    if (!od_xfer_app_begin_full(&state->geometry)) {
        od_xfer_fail_active(OD_XFER_TERM_PANEL_PREPARATION, "START", -1, 0u, 0u,
                            OD_XFER_ABORT_START_FAILED, false);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_START_ACK);
        return OD_CMD_NACK;
    }
    if (compressed) {
        od_span_t inline_input = od_span_drop(body, 4u);
        od_xfer_stream_reset(expected);
        if (inline_input.n > 0u) {
            od_xfer_stream_result_t result;
            state->received_bytes = (uint32_t)inline_input.n;
            result = od_xfer_stream_push(inline_input, false);
            if (result != OD_XFER_STREAM_OK) {
                od_xfer_fail_active(od_xfer_stream_cause(result), "START", -1,
                                    state->written_bytes, (uint32_t)inline_input.n,
                                    OD_XFER_ABORT_START_FAILED, false);
                od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_START_ACK);
                return OD_CMD_NACK;
            }
        }
    }
    if (od_xfer_reply_app(ctx, ack, (uint16_t)sizeof ack) != OD_TXQ_OK) {
        od_xfer_fail_active(OD_XFER_TERM_REPLY_DELIVERY, "START", -1, 0u, 0u,
                            OD_XFER_ABORT_REPLY_FAILED, false);
        return OD_CMD_NACK;
    }
    return OD_CMD_OK;
}

/* Gated on the capability, not built everywhere: only a target that routes 0x0085 can reach
 * this, and the transfer plane is otherwise identical for all of them. */
#if OD_CAP_DRIVE_WRITE
bool od_xfer_adopt_full(const od_reply_t *owner, const od_color_geometry_t *geometry,
                        uint32_t expected_bytes, bool compressed, od_span_t inline_data)
{
    od_xfer_state_t *state = od_xfer_state();

    if (owner == NULL || geometry == NULL || expected_bytes == 0u) {
        return false;
    }
    /* No od_xfer_replace_active()/od_xfer_app_prepare_start() here: the app aborted whatever was
     * in flight BEFORE it powered the panel, because it had to own the hardware to select a
     * mode at all. Doing it again would tear down the transfer this call is registering. */
    od_xfer_clear_state();
    state->mode = OD_XFER_DIRECT_FULL;
    state->owner = *owner;
    state->started_ms = od_xfer_app_now_ms();
    state->expected_bytes = expected_bytes;
    state->compressed = compressed;
    state->geometry = *geometry;

    od_xfer_log_start();
    if (compressed) {
        od_xfer_stream_reset(expected_bytes);
        if (inline_data.n > 0u) {
            od_xfer_stream_result_t result;
            state->received_bytes = (uint32_t)inline_data.n;
            result = od_xfer_stream_push(inline_data, false);
            if (result != OD_XFER_STREAM_OK) {
                od_xfer_fail_active(od_xfer_stream_cause(result), "START", -1,
                                    state->written_bytes, (uint32_t)inline_data.n,
                                    OD_XFER_ABORT_START_FAILED, false);
                return false;
            }
        }
    } else if (inline_data.n > 0u) {
        /* An uncompressed START may also carry the first bytes. od_xfer_app_write is the same
         * path DATA takes, so the pass-boundary handling applies to them too. */
        state->received_bytes = (uint32_t)inline_data.n;
        if (od_xfer_app_write(0u, inline_data) != (uint32_t)inline_data.n) {
            od_xfer_fail_active(OD_XFER_TERM_PANEL_WRITE, "START", -1, state->written_bytes,
                                (uint32_t)inline_data.n, OD_XFER_ABORT_START_FAILED, false);
            return false;
        }
        state->written_bytes = (uint32_t)inline_data.n;
    }
    return true;
}
#endif /* OD_CAP_DRIVE_WRITE */

od_cmd_result_t od_xfer_direct_data_impl(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_state_t *state = od_xfer_state();
    const uint8_t ack[] = { RESP_ACK, RESP_DIRECT_WRITE_DATA_ACK };

    if (!od_xfer_owner_matches(ctx)) {
        od_xfer_log_owner_mismatch(CMD_DIRECT_WRITE_DATA);
        return OD_CMD_OK;
    }
    if (body.n == 0u) {
        return OD_CMD_OK;
    }
    if (body.n > UINT32_MAX) {
        od_xfer_fail_active(OD_XFER_TERM_SIZE_EXCEEDED, "DATA", -1, 0u, UINT32_MAX,
                            OD_XFER_ABORT_STREAM_FAILED, false);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_DATA_ACK);
        return OD_CMD_NACK;
    }
    od_xfer_log_chunk(body);
    if (body.n > UINT32_MAX - state->received_bytes) {
        od_xfer_fail_active(OD_XFER_TERM_SIZE_EXCEEDED, "DATA", -1, 0u,
                            (uint32_t)body.n, OD_XFER_ABORT_STREAM_FAILED, false);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_DATA_ACK);
        return OD_CMD_NACK;
    }
    state->received_bytes += (uint32_t)body.n;
    if (state->compressed) {
        const od_xfer_stream_result_t result = od_xfer_stream_push(body, false);
        if (result != OD_XFER_STREAM_OK) {
            od_xfer_fail_active(od_xfer_stream_cause(result), "DATA", -1,
                                state->written_bytes, (uint32_t)body.n,
                                OD_XFER_ABORT_STREAM_FAILED, false);
            od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_DATA_ACK);
            return OD_CMD_NACK;
        }
        od_xfer_log_progress();
    } else {
        uint32_t remaining = state->written_bytes < state->expected_bytes
            ? state->expected_bytes - state->written_bytes : 0u;
        const uint32_t offered = body.n < remaining ? (uint32_t)body.n : remaining;

        if (offered > 0u) {
            const od_span_t prefix = od_span_take(body, offered);
            const uint32_t offset = state->written_bytes;
            const uint32_t consumed = od_xfer_app_write(state->written_bytes, prefix);
            if (consumed != offered) {
                od_xfer_fail_active(OD_XFER_TERM_PANEL_WRITE, "DATA", -1, offset, offered,
                                    OD_XFER_ABORT_STREAM_FAILED, false);
                od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_DATA_ACK);
                return OD_CMD_NACK;
            }
            state->written_bytes += consumed;
        }
        od_xfer_log_progress();
#if OD_XFER_DIRECT_AUTO_END
        if (state->written_bytes >= state->expected_bytes) {
            return od_xfer_direct_end_impl(ctx, od_span_none());
        }
#endif
    }
    if (od_xfer_reply_app(ctx, ack, (uint16_t)sizeof ack) != OD_TXQ_OK) {
        od_xfer_fail_active(OD_XFER_TERM_REPLY_DELIVERY, "DATA", -1, 0u, 0u,
                            OD_XFER_ABORT_REPLY_FAILED, false);
        return OD_CMD_NACK;
    }
    return OD_CMD_OK;
}

static od_cmd_result_t finish_refresh(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_state_t *state = od_xfer_state();

    state->finalized_ms = od_xfer_app_now_ms();
    state->finalized = true;

    const od_reply_t owner = state->owner;
    const uint8_t ack[] = { RESP_ACK, RESP_DIRECT_WRITE_END_ACK };
    const uint8_t success[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_SUCCESS };
    const uint8_t timeout[] = { RESP_ACK, RESP_DIRECT_WRITE_REFRESH_TIMEOUT };
    const bool has_etag = body.n >= 5u;
    const uint32_t new_etag = has_etag ? read_be32(body.p + 1u) : 0u;
    const uint8_t refresh_mode = body.n > 0u && body.p[0] == OD_REFRESH_FAST
        ? OD_REFRESH_FAST : OD_REFRESH_FULL;
    bool completed = false;
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    od_xfer_terminal_snapshot_t snapshot;
#endif

    if (od_xfer_reply_app(ctx, ack, (uint16_t)sizeof ack) != OD_TXQ_OK) {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
        od_xfer_terminal_capture(&snapshot);
        od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_REPLY_DELIVERY,
                                 "END", -1, 0u, 0u);
#endif
        od_xfer_clear_state();
        od_xfer_app_barrier_abort(&owner);
        return OD_CMD_NACK;
    }
    if (od_xfer_app_before_refresh(&owner) != OD_XFER_BARRIER_PROCEED) {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
        od_xfer_terminal_capture(&snapshot);
        od_xfer_terminal_failure(&snapshot, OD_XFER_TERM_BARRIER_ABORTED,
                                 "END", -1, 0u, 0u);
#endif
        od_xfer_clear_state();
        od_xfer_app_barrier_abort(&owner);
        return OD_CMD_NACK;
    }
    if (!od_xfer_app_refresh(refresh_mode, &completed)) {
        od_xfer_fail_active(OD_XFER_TERM_REFRESH_FAILED, "END", -1, 0u, 0u,
                            OD_XFER_ABORT_REFRESH_FAILED, false);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_END_ACK);
        return OD_CMD_NACK;
    }
#if OD_CAP_PARTIAL
    if (completed) {
        od_xfer_app_set_displayed_etag(has_etag && new_etag != 0u ? new_etag : 0u);
    } else if (has_etag) {
        od_xfer_app_set_displayed_etag(0u);
    }
#else
    (void)has_etag;
    (void)new_etag;
#endif
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_ERROR
    od_xfer_terminal_capture(&snapshot);
#endif
    od_xfer_clear_state();
    if (od_xfer_reply_app(ctx, completed ? success : timeout,
                          (uint16_t)(completed ? sizeof success : sizeof timeout)) != OD_TXQ_OK) {
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

od_cmd_result_t od_xfer_direct_end_impl(const od_cmd_ctx_t *ctx, od_span_t body)
{
    od_xfer_state_t *state = od_xfer_state();

    if (!od_xfer_owner_matches(ctx)) {
        od_xfer_log_owner_mismatch(CMD_DIRECT_WRITE_END);
        return OD_CMD_OK;
    }
    if (state->compressed) {
        const od_xfer_stream_result_t result = od_xfer_stream_push(od_span_none(), true);
        if (result != OD_XFER_STREAM_OK) {
            od_xfer_fail_active(od_xfer_stream_cause(result), "END", -1,
                                state->written_bytes, 0u,
                                OD_XFER_ABORT_STREAM_FAILED, false);
            od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_END_ACK);
            return OD_CMD_NACK;
        }
    } else if (state->geometry.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
               && state->written_bytes != state->expected_bytes) {
        od_xfer_fail_active(OD_XFER_TERM_INCOMPLETE, "END", -1, 0u, 0u,
                            OD_XFER_ABORT_INCOMPLETE, false);
        od_xfer_reply_simple_error(ctx, RESP_DIRECT_WRITE_END_ACK);
        return OD_CMD_NACK;
    }
    return finish_refresh(ctx, body);
}
