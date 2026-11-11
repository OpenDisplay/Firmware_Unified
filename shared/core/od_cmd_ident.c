/* CMD_READ_IDENT (0x0046), one implementation for every target. Plaintext, pre-gate: answered
 * from DisplayConfig 0x20. display_count = 0 is a valid answer (reachable, no display).
 *
 * After the records: [flags][max_config:2][max_seq_steps:2][escapes] (opendisplay_protocol.h),
 * so a host needs no per-target table of limits. A host reading only count * 7 bytes never sees
 * the tail. */
#include "od_cmd_app.h"

#include "od_cfg_index.h"
#include "od_config.h"
#include "od_config_asm.h"
#include "od_epd.h"
#include "od_ident_app.h"
#include "od_reply.h"
#include "od_sec2.h"
#include "opendisplay_protocol.h"

#include <stddef.h>

#if OD_CAP_READ_IDENT

#define IDENT_HDR 5u
#define IDENT_REC 7u
#define IDENT_TAIL 6u

/* This firmware drives panels only from drive packets (0xAE-0xB1). A target still carrying a
 * panel library builds with OD_IDENT_CONFIG_DRIVEN=0. */
#ifndef OD_IDENT_CONFIG_DRIVEN
#define OD_IDENT_CONFIG_DRIVEN 1
#endif

#define IDENT_MAX_CONFIG \
    (OD_CONFIG_MAX_SIZE < OD_CONFIG_ASM_MAX_TRANSFERABLE ? OD_CONFIG_MAX_SIZE \
                                                         : OD_CONFIG_ASM_MAX_TRANSFERABLE)
#define IDENT_ESCAPES (OD_EPD_WITH_WINDOW ? (OD_IDENT_ESC_WINDOW | OD_IDENT_ESC_FILL) : 0u)

od_cmd_result_t od_cmd_app_read_ident(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const struct od_config *cfg = od_ident_app_config();
    uint8_t rsp[IDENT_HDR + OD_CONFIG_MAX_DISPLAYS * IDENT_REC + IDENT_TAIL];
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
#if OD_IDENT_NATIVE_PANELS
            if (od_ident_app_native_panel(d)) {
                flags |= OD_IDENT_FLAG_BUILTIN_DRIVER;
                continue;
            }
#endif
            if (OD_IDENT_CONFIG_DRIVEN &&
                !od_cfg_has_drive_data(d->instance_number, d->color_scheme)) {
                flags |= OD_IDENT_FLAG_NEEDS_MIGRATION;
            }
        }
    }
    rsp[4] = count;
    flags |= od_sec2_ident_flags();   /* SECURITY_V2 5.2: SEC_V2, CLAIMED, CLAIM_MODE, LEGACY_SECRETS */
    rsp[n++] = flags;
    rsp[n++] = (uint8_t)(IDENT_MAX_CONFIG & 0xFFu);
    rsp[n++] = (uint8_t)(IDENT_MAX_CONFIG >> 8);
    rsp[n++] = (uint8_t)(OD_EPD_MAX_SEQ_STEPS & 0xFFu);
    rsp[n++] = (uint8_t)(OD_EPD_MAX_SEQ_STEPS >> 8);
    rsp[n++] = (uint8_t)IDENT_ESCAPES;
    (void)od_reply_plain(ctx->r, &ctx->rp, rsp, n);
    return OD_CMD_OK;
}

#endif /* OD_CAP_READ_IDENT */
