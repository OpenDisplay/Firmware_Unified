/* CMD_DRIVE_WRITE_START (0x0085) -- the wire half.
 *
 * A full-frame write that NAMES a drive mode declared in the config, so a multi-pass mode is
 * ONE transfer rather than one per pass. 16-colour BWRY needs three passes whose pixel codes
 * differ per pass, which the ordinary 0x0070 path cannot express: the mode has to be known at
 * START because this driver uploads the waveform as part of the init sequence, before the frame.
 *
 * THIS FILE IS THE DISPATCH HALF. od_display_drive_write_start() selects the mode, enforces its
 * temp_policy and powers the glass for pass 0; od_xfer_adopt_full() then registers the transfer
 * so DATA (0x0071) and END (0x0072) continue it with no drive-specific code of their own.
 */
#include "od_cmd_app.h"

#include "od_display_drive.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "opendisplay_display.h"
#include "opendisplay_protocol.h"

#include <stddef.h>

#if OD_CAP_DRIVE_WRITE

/* [mode][refresh][flags][rsv:2] before any payload tail. */
#define DRIVE_START_HDR 5u

/* The target's own reply shape, as od_cmd_silabs.c and od_cmd_flash.c have it. */
static od_txq_status_t reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    return od_reply(ctx->r, &ctx->rp, frame, len);
}

/* [0xFF][0x85][err][mode] -- the mode byte is the one the host ASKED for, so a NACK is
 * attributable even when the device would have substituted a different mode. */
static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint8_t err, uint8_t mode)
{
    const uint8_t r[4] = { RESP_NACK, RESP_DRIVE_WRITE_START, err, mode };

    (void)reply(ctx, r, sizeof r);
    return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_drive_write_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t passes = 0u;
    uint8_t mode_used;
    int8_t  temp_c = (int8_t)0x7F;
    uint8_t r[5];
    int rc;

    if (body.p == NULL || body.n < DRIVE_START_HDR) {
        return nack(ctx, OD_ERR_DRIVE_BAD_ARGS, 0x00u);
    }
    /* Reserved bytes are REJECTED, not ignored: accepting junk now means a future host cannot
     * tell whether this device understood a field it set. */
    if (body.p[3] != 0u || body.p[4] != 0u) {
        return nack(ctx, OD_ERR_DRIVE_BAD_ARGS, body.p[0]);
    }

    mode_used = body.p[0];
    rc = od_display_drive_write_start(body.p[0], body.p[1], body.p[2],
                                      body.p + DRIVE_START_HDR,
                                      (uint16_t)(body.n - DRIVE_START_HDR),
                                      &passes, &mode_used, &temp_c);
    if (rc != 0) {
        return nack(ctx, (uint8_t)(-rc), body.p[0]);
    }

    /* The panel is up at the chosen mode and the app knows passes*frame; hand that to the
     * shared transfer layer so DATA (0x0071) and END (0x0072) continue it unchanged. Adopt
     * rather than start: the hardware is already running. */
    {
        od_color_geometry_t geom;
        uint32_t expected = 0u;
        const bool compressed = (body.p[2] & DRIVE_WRITE_FLAG_COMPRESSED) != 0u;
        od_span_t tail = od_span_drop(body, DRIVE_START_HDR);

        if (!opendisplay_display_drive_stream_info(&geom, &expected)) {
            return nack(ctx, OD_ERR_DRIVE_NO_CONFIG, body.p[0]);
        }
        /* The 4-byte uncompressed_size is a START field, not stream data. */
        if (compressed) {
            tail = od_span_drop(tail, 4u);
        }
        if (!od_xfer_adopt_full(&ctx->rp, &geom, expected, compressed, tail)) {
            return nack(ctx, OD_ERR_DRIVE_BAD_SIZE, body.p[0]);
        }
    }

    /* ACK carries what the device DECIDED, not an echo of the request: `mode` is the mode
     * actually selected (temp_policy SUBSTITUTE may pick another), `passes` comes from the
     * config so the host need not duplicate the mode table, and temp_c is the device's own
     * estimate (0x7F = unknown). A host must read these back rather than assume. */
    r[0] = RESP_ACK;
    r[1] = RESP_DRIVE_WRITE_START;
    r[2] = mode_used;
    r[3] = passes;
    r[4] = (uint8_t)temp_c;
    (void)reply(ctx, r, sizeof r);
    return OD_CMD_OK;
}

#endif /* OD_CAP_DRIVE_WRITE */
