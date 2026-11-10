/* CMD_READ_IDENT (0x0046), one implementation for every target. Plaintext, pre-gate: answered
 * from DisplayConfig 0x20. display_count = 0 is a valid answer (reachable, no display). */
#include "od_cmd_app.h"

#include "od_config.h"
#include "od_ident_app.h"
#include "od_reply.h"
#include "opendisplay_protocol.h"

#include <stddef.h>

#if OD_CAP_READ_IDENT

#define IDENT_HDR 5u
#define IDENT_REC 7u

od_cmd_result_t od_cmd_app_read_ident(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const struct od_config *cfg = od_ident_app_config();
    uint8_t rsp[IDENT_HDR + OD_CONFIG_MAX_DISPLAYS * IDENT_REC];
    uint16_t n = IDENT_HDR;
    uint8_t count = 0u;

    (void)body;
    rsp[0] = RESP_ACK;
    rsp[1] = RESP_READ_IDENT;
    rsp[2] = (uint8_t)OD_PROTOCOL_VERSION_MINOR;
    rsp[3] = (uint8_t)OD_IDENT_TARGET;
    if (cfg != NULL && cfg->loaded) {
        for (uint8_t i = 0u; i < cfg->display_count && i < OD_CONFIG_MAX_DISPLAYS; i++) {
            const struct DisplayConfig *d = &cfg->displays[i];

            rsp[n++] = (uint8_t)(d->panel_ic_type & 0xFFu);
            rsp[n++] = (uint8_t)(d->panel_ic_type >> 8);
            rsp[n++] = (uint8_t)(d->pixel_width & 0xFFu);
            rsp[n++] = (uint8_t)(d->pixel_width >> 8);
            rsp[n++] = (uint8_t)(d->pixel_height & 0xFFu);
            rsp[n++] = (uint8_t)(d->pixel_height >> 8);
            rsp[n++] = d->color_scheme;
            count++;
        }
    }
    rsp[4] = count;
    (void)od_reply_plain(ctx->r, &ctx->rp, rsp, n);
    return OD_CMD_OK;
}

#endif /* OD_CAP_READ_IDENT */
