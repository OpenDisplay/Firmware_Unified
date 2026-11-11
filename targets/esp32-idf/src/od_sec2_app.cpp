/* od_sec2_app.cpp -- this target's half of shared/core/od_sec2_app.h, plus od_txq's drop seam. */

#include "od_sec2_app.h"
#include "od_sec2_wire.h"
#include "od_txq.h"   /* od_reply_t / od_radio_result_t, for the drop seam at the end */

#include "encryption_state.h"
#include "od_hal_time.h"
#include "od_config.h"
#include "structs.h"

extern struct od_config globalConfig;

#include "esp_mac.h"

#include <string.h>

extern "C" uint32_t od_sec2_app_now_ms(void)
{
    return od_hal_uptime_ms();
}

/* SECURITY_V2 4.3: 0x00 0x00 then the six bytes esp_efuse_mac_get_default() writes, mac[0]
 * first. Zeroed first so a failed read is the same answer every boot. */
extern "C" void od_sec2_app_uid(uint8_t uid[8])
{
    uint8_t mac[6] = {0};

    (void)esp_efuse_mac_get_default(mac);
    uid[0] = 0u;
    uid[1] = 0u;
    memcpy(&uid[2], mac, sizeof mac);
}

extern "C" uint8_t od_sec2_app_dflags(void)
{
    uint8_t f = 0u;

    if (globalConfig.loaded && globalConfig.display_count > 0u) {
        f |= OD_SEC2_DFLAG_HAS_PANEL;
    }
    if ((securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) != 0u) {
        f |= OD_SEC2_DFLAG_HAS_RESET_PIN;
    }
    return f;
}

extern "C" const struct SecurityConfig *od_sec2_app_security(void)
{
    return &securityConfig;
}

/* od_txq's drop seam, empty here. od_txq.c logs the discard itself at INFO, because a normal
 * disconnect mid-upload discards every frame still queued for that link. */
extern "C" void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp;
    (void)len;
    (void)why;
}
