/* CMD_READ_IDENT (0x0046), one implementation for every target. Plaintext, pre-gate: answered
 * from DisplayConfig 0x20. display_count = 0 is a valid answer (reachable, no display).
 *
 * One flags byte follows the records (docs/CONFIG_DRIVEN_DISPLAY.md), provisional until
 * opendisplay-protocol takes it: a host reading only count * 7 bytes never sees it. */
#include "od_cmd_app.h"

#include "od_cfg_index.h"
#include "od_config.h"
#include "od_ident_app.h"
#include "od_reply.h"
#include "opendisplay_protocol.h"

#include <stddef.h>

#if OD_CAP_READ_IDENT

#define IDENT_HDR 5u
#define IDENT_REC 7u

/* This firmware drives panels only from drive packets (0xAE-0xB1). A target still carrying a
 * panel library builds with OD_IDENT_CONFIG_DRIVEN=0. */
#ifndef OD_IDENT_CONFIG_DRIVEN
#define OD_IDENT_CONFIG_DRIVEN 1
#endif
#define OD_IDENT_FLAG_CONFIG_DRIVEN   0x01u
/* A configured display has no drive packets: nothing will be drawn until a config carrying
 * them is written. */
#define OD_IDENT_FLAG_NEEDS_MIGRATION 0x02u

od_cmd_result_t od_cmd_app_read_ident(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const struct od_config *cfg = od_ident_app_config();
    uint8_t rsp[IDENT_HDR + OD_CONFIG_MAX_DISPLAYS * IDENT_REC + 1u];
    uint8_t flags = OD_IDENT_CONFIG_DRIVEN ? OD_IDENT_FLAG_CONFIG_DRIVEN : 0u;
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
            if (OD_IDENT_CONFIG_DRIVEN && !od_cfg_has_drive_data(d->instance_number, d->color_scheme)
#if OD_IDENT_NATIVE_PANELS
                && !od_ident_app_native_panel(d)
#endif
            ) {
                flags |= OD_IDENT_FLAG_NEEDS_MIGRATION;
            }
        }
    }
    rsp[4] = count;
    rsp[n++] = flags;
    (void)od_reply_plain(ctx->r, &ctx->rp, rsp, n);
    return OD_CMD_OK;
}

#endif /* OD_CAP_READ_IDENT */
