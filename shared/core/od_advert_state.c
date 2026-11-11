/* od_advert_device_state_now() and the drivable-display predicate (od_advert.h, od_ident_app.h).
 * Not in od_advert.c, which is the PURE tier: this reads the security state and the config index.
 * Lives in the APP_IDENT tier, which every target composes alongside APP_SESSION and APP_EPD. */
#include "od_advert.h"

#include "od_cfg_index.h"
#include "od_claim_mode.h"
#include "od_config.h"
#include "od_ident_app.h"
#include "od_sec2.h"

#include <string.h>

/* Same default as od_cmd_ident.c: drive packets are the only way to drive a panel. */
#ifndef OD_IDENT_CONFIG_DRIVEN
#define OD_IDENT_CONFIG_DRIVEN 1
#endif

bool od_ident_display_drivable(const struct od_config *cfg)
{
    if (cfg == NULL || !cfg->loaded) {
        return false;
    }
    for (uint8_t i = 0u; i < cfg->display_count && i < OD_CONFIG_MAX_DISPLAYS; i++) {
        const struct DisplayConfig *d = &cfg->displays[i];

#if OD_IDENT_NATIVE_PANELS
        if (od_ident_app_native_panel(d)) {
            return true;
        }
#endif
        if (!OD_IDENT_CONFIG_DRIVEN ||
            od_cfg_has_drive_data(d->instance_number, d->color_scheme)) {
            return true;
        }
    }
    return false;
}

void od_advert_device_state_now(struct od_advert_device_state *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->claim_mode = od_claim_mode_active();
    out->session_required = od_sec2_needs_session();
    out->display_drivable = od_ident_display_drivable(od_ident_app_config());
}
