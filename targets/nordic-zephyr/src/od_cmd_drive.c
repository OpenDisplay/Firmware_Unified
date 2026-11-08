/* CMD_DRIVE_WRITE_START (0x0085) on Nordic.
 *
 * A full-frame write naming a config-declared drive mode, so a multi-pass mode is one transfer.
 * This selects the mode, applies its temp_policy and brings the panel up for pass 0; the shared
 * transfer layer then carries DATA (0x0071) and END (0x0072). Wire behaviour matches
 * targets/efr32bg22-slc/od_cmd_drive.c and od_display_drive.cpp.
 */
#include "od_cmd_app.h"

#include "od_color.h"
#include "od_epd.h"
#include "od_epd_nordic.h"
#include "od_log.h"
#include "od_reply.h"
#include "od_xfer.h"
#include "opendisplay_ble.h"
#include "opendisplay_display.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include <stddef.h>

#if OD_CAP_DRIVE_WRITE

/* [mode][refresh][flags][rsv:2] before any payload tail. */
#define DRIVE_START_HDR 5u
#define TEMP_UNKNOWN    ((int8_t)0x7F)

static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint8_t err, uint8_t mode)
{
    const uint8_t r[4] = { RESP_NACK, RESP_DRIVE_WRITE_START, err, mode };

    (void)od_reply(ctx->r, &ctx->rp, r, sizeof r);
    return OD_CMD_NACK;
}

/* The die sensor, an estimate of the glass for policy purposes. -999 means never read. */
static int8_t drive_temp_c(void)
{
    const float t = opendisplay_ble_get_chip_temperature();

    if (t < -128.0f || t > 126.0f) {
        return TEMP_UNKNOWN;
    }
    return (int8_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
}

/* min == max == 0 is the zero-filled "no window declared". */
static bool mode_temp_ok(const od_epd_nordic_mode_t *m, int8_t t)
{
    return (m->temp_min_c == 0 && m->temp_max_c == 0) ||
           (t >= m->temp_min_c && t <= m->temp_max_c);
}

/* Returns 0, or an OD_ERR_DRIVE_* code with the panel left down. */
static uint8_t drive_begin(uint8_t *mode, uint8_t flags, od_span_t tail, uint8_t *passes,
                           int8_t *temp_c, od_color_geometry_t *geom)
{
    const struct DisplayConfig *d = opendisplay_display_config();
    od_epd_nordic_mode_t hdr;

    /* The windowed path is not implemented for a config-declared mode. */
    if ((flags & DRIVE_WRITE_FLAG_PIPELINED) != 0u) {
        return OD_ERR_DRIVE_UNSUPPORTED;
    }
    if (d == NULL || !od_epd_nordic_load(d)) {
        return OD_ERR_DRIVE_NO_CONFIG;
    }
    opendisplay_display_abort();

    if (!od_epd_nordic_select(*mode, &hdr)) {
        return OD_ERR_DRIVE_BAD_MODE;
    }
    *temp_c = drive_temp_c();
    if (*temp_c != TEMP_UNKNOWN && !mode_temp_ok(&hdr, *temp_c)) {
        if (hdr.temp_policy == OD_DRIVE_TEMP_REJECT) {
            return OD_ERR_DRIVE_TEMP_RANGE;
        }
        if (hdr.temp_policy == OD_DRIVE_TEMP_SUBSTITUTE) {
            const uint8_t count = od_epd_nordic_mode_count();
            uint8_t i;

            for (i = 0u; i < count; i++) {
                if (od_epd_nordic_select(i, &hdr) && mode_temp_ok(&hdr, *temp_c)) {
                    break;
                }
            }
            if (i >= count) {
                return OD_ERR_DRIVE_TEMP_RANGE;
            }
            *mode = i;
        }
    }
    if (od_color_direct_geometry(hdr.color_scheme == 0xFFu ? d->color_scheme : hdr.color_scheme,
                                 d->pixel_width, d->pixel_height, geom) != OD_COLOR_OK) {
        return OD_ERR_DRIVE_NO_CONFIG;
    }
    if (!opendisplay_display_drive_panel_up(d)) {
        return OD_ERR_DRIVE_NO_CONFIG;
    }
    if (!od_epd_nordic_begin_pass(0u)) {
        const bool unsupported = od_epd_nordic_last_rc() == OD_EPD_RC_UNSUP;

        opendisplay_display_drive_panel_down();
        return unsupported ? OD_ERR_DRIVE_SEQ_UNSUPPORTED : OD_ERR_DRIVE_NO_CONFIG;
    }
    if (!opendisplay_display_drive_adopt(geom, hdr.passes,
                                         (flags & DRIVE_WRITE_FLAG_COMPRESSED) != 0u,
                                         tail.p, (uint16_t)tail.n)) {
        opendisplay_display_abort();
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
    int8_t temp_c = TEMP_UNKNOWN;
    od_color_geometry_t geom;
    uint32_t expected;
    uint8_t r[5];

    if (body.p == NULL || body.n < DRIVE_START_HDR) {
        return nack(ctx, OD_ERR_DRIVE_BAD_ARGS, 0x00u);
    }
    if (body.p[3] != 0u || body.p[4] != 0u) {
        return nack(ctx, OD_ERR_DRIVE_BAD_ARGS, body.p[0]);
    }
    mode = body.p[0];
    const bool compressed = (body.p[2] & DRIVE_WRITE_FLAG_COMPRESSED) != 0u;
    od_span_t tail = od_span_drop(body, DRIVE_START_HDR);

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
