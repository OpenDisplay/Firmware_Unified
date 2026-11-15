/* CMD_DRIVE_WRITE_START (0x0085) for targets that run od_epd through od_epd_cfg.
 *
 * A full-frame write naming a config-declared drive mode, so a multi-pass mode is one transfer.
 * This selects the mode, applies its temp_policy and brings the panel up for pass 0; the shared
 * transfer layer then carries DATA (0x0071) and END (0x0072). Wire behaviour matches
 * targets/efr32bg22-slc/od_cmd_drive.c, which keeps its own copy over its own panel adapter.
 */
#include "od_cmd_app.h"

#include "od_color.h"
#include "od_drive_app.h"
#include "od_epd.h"
#include "od_epd_cfg.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <stddef.h>

#if OD_CAP_DRIVE_WRITE

/* [mode][refresh][flags][rsv:2] before any payload tail. */
#define DRIVE_START_HDR 5u

static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint8_t err, uint8_t mode)
{
    const uint8_t r[4] = { RESP_NACK, RESP_DRIVE_WRITE_START, err, mode };

    (void)od_reply(ctx->r, &ctx->rp, r, sizeof r);
    return OD_CMD_NACK;
}

/* Returns 0, or an OD_ERR_DRIVE_* code with the panel left down. */
static uint8_t drive_begin(uint8_t *mode, uint8_t flags, od_span_t tail, uint8_t *passes,
                           int8_t *temp_c, od_color_geometry_t *geom)
{
    const struct DisplayConfig *d = od_drive_app_display();
    od_epd_cfg_mode_t hdr;

    /* The windowed path is not implemented for a config-declared mode. */
    if ((flags & DRIVE_WRITE_FLAG_PIPELINED) != 0u) {
        return OD_ERR_DRIVE_UNSUPPORTED;
    }
    /* Abort first: it may sleep a previous od_epd_cfg session, which must not run against the
     * descriptor this load is about to replace. The transfer that owned that panel ends with
     * it, accepted or refused: the app abort does not touch od_xfer, and a transfer left
     * active with no panel behind it defers the display service until it times out. */
    od_xfer_replace_active();
    od_drive_app_abort();
    if (d == NULL || !od_epd_cfg_load(d, od_drive_app_bus())) {
        return OD_ERR_DRIVE_NO_CONFIG;
    }

    *temp_c = od_drive_app_temp_c();
    {
        const uint8_t err = od_epd_cfg_select_policy(mode, *temp_c, &hdr);
        if (err != 0u) {
            return err;
        }
    }
    if (od_color_direct_geometry(hdr.color_scheme == 0xFFu ? d->color_scheme : hdr.color_scheme,
                                 d->pixel_width, d->pixel_height, geom) != OD_COLOR_OK) {
        return OD_ERR_DRIVE_NO_CONFIG;
    }
    if (!od_drive_app_panel_up(d)) {
        return OD_ERR_DRIVE_NO_CONFIG;
    }
    if (!od_epd_cfg_begin_pass(0u)) {
        const int rc = od_epd_cfg_last_rc();

        od_drive_app_panel_down();
        return rc == OD_EPD_RC_UNSUP ? OD_ERR_DRIVE_SEQ_UNSUPPORTED
             : rc == OD_EPD_RC_BUSY  ? OD_ERR_DRIVE_PANEL_BUSY : OD_ERR_DRIVE_NO_CONFIG;
    }
    if (!od_drive_app_adopt(geom, hdr.passes, (flags & DRIVE_WRITE_FLAG_COMPRESSED) != 0u,
                            tail)) {
        od_drive_app_abort();
        return OD_ERR_DRIVE_BAD_SIZE;
    }
    *passes = hdr.passes;
    return 0u;
}

od_cmd_result_t od_cmd_app_drive_write_start(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t passes = 0u;
    uint8_t mode;
    uint8_t err;
    int8_t temp_c = OD_DRIVE_TEMP_UNKNOWN;
    od_color_geometry_t geom;
    uint32_t expected;
    bool compressed;
    od_span_t tail;
    uint8_t r[5];

    if (body.p == NULL || body.n < DRIVE_START_HDR) {
        return nack(ctx, OD_ERR_DRIVE_BAD_ARGS, 0x00u);
    }
    if (body.p[3] != 0u || body.p[4] != 0u) {
        return nack(ctx, OD_ERR_DRIVE_BAD_ARGS, body.p[0]);
    }
    mode = body.p[0];
    compressed = (body.p[2] & DRIVE_WRITE_FLAG_COMPRESSED) != 0u;
    tail = od_span_drop(body, DRIVE_START_HDR);

    err = drive_begin(&mode, body.p[2], tail, &passes, &temp_c, &geom);
    if (err != 0u) {
        return nack(ctx, err, body.p[0]);
    }
    expected = geom.total_bytes * passes;
    geom.total_bytes = expected;
    /* The 4-byte uncompressed_size is a START field, not stream data. */
    if (compressed) {
        tail = od_span_drop(tail, 4u);
    }
    if (!od_xfer_adopt_full(&ctx->rp, &geom, expected, compressed, tail)) {
        od_drive_app_abort();
        return nack(ctx, OD_ERR_DRIVE_BAD_SIZE, body.p[0]);
    }

    r[0] = RESP_ACK;
    r[1] = RESP_DRIVE_WRITE_START;
    r[2] = mode;
    r[3] = passes;
    r[4] = (uint8_t)temp_c;
    (void)od_reply(ctx->r, &ctx->rp, r, sizeof r);
    return OD_CMD_OK;
}

#endif /* OD_CAP_DRIVE_WRITE */
