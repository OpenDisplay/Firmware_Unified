/* od_net_g.c -- see od_net_g.h. Network specification here, stack calls behind od_hal_adv2. */
#include "od_net_g.h"

#include <string.h>

#include "od_hal_adv2.h"
#include "od_log.h"

/* See od_net_g.h: the value the 2.3.0 fleet advertises. */
#define OD_NET_G_HASHED_FLAGS 0x00u

static bool s_started;

bool od_net_g_config_active(const struct FindMyConfig *cfg)
{
    if (cfg == NULL || (cfg->flags & OD_FINDMY_FLAG_FMDN_ENABLE) == 0u
        || cfg->fmdn_curve != OD_FINDMY_CURVE_SECP160R1) {
        return false;
    }
    for (uint32_t i = 0u; i < OD_NET_G_EID_LEN; i++) {
        if (cfg->advertisement_key[i] != 0u) {
            return true;
        }
    }
    return false;
}

void od_net_g_derive_address(const uint8_t eid[OD_NET_G_EID_LEN], uint8_t addr[6])
{
    /* Same derivation as network A: top six bytes, MSB first, two high bits forced to 0b11. */
    addr[5] = (uint8_t)(eid[0] | 0xC0u);
    addr[4] = eid[1];
    addr[3] = eid[2];
    addr[2] = eid[3];
    addr[1] = eid[4];
    addr[0] = eid[5];
}

uint8_t od_net_g_build_adv(const uint8_t eid[OD_NET_G_EID_LEN], uint8_t *adv, uint8_t cap)
{
    if (adv == NULL || cap < OD_NET_G_LEGACY_ADV_LEN) {
        return 0u;
    }
    /*   [0..2]   02 01 06       flags AD
     *   [3]      0x19           length: 25 bytes follow
     *   [4]      0x16           service data, 16-bit UUID
     *   [5..6]   AA FE          UUID 0xFEAA, little-endian
     *   [7]      0x41           frame type (static UTP frame)
     *   [8..27]  eid[0..19]     the full EID; unlike network A nothing is moved to the address
     *   [28]     hashed flags */
    adv[0] = 0x02u;
    adv[1] = 0x01u;
    adv[2] = 0x06u;
    adv[3] = 0x19u;
    adv[4] = 0x16u;
    adv[5] = 0xAAu;
    adv[6] = 0xFEu;
    adv[7] = OD_NET_G_FRAME_TYPE_UTP;
    memcpy(&adv[8], eid, OD_NET_G_EID_LEN);
    adv[28] = OD_NET_G_HASHED_FLAGS;
    return OD_NET_G_LEGACY_ADV_LEN;
}

void od_net_g_invalidate(void)
{
    s_started = false;
}

void od_net_g_sync(const struct FindMyConfig *cfg)
{
    uint8_t adv[OD_NET_G_LEGACY_ADV_LEN];
    uint8_t addr[6];

    if (!od_hal_adv2_supported()) {
        return;
    }
    if (!od_net_g_config_active(cfg)) {
        if (s_started) {
            (void)od_hal_adv2_stop(OD_HAL_ADV2_SET_NET_G);
            s_started = false;
        }
        return;
    }
    if (s_started) {
        return;
    }

    /* Stop before reprogramming: the address belongs to the set. */
    (void)od_hal_adv2_stop(OD_HAL_ADV2_SET_NET_G);
    (void)od_net_g_build_adv(cfg->advertisement_key, adv, (uint8_t)sizeof adv);
    od_net_g_derive_address(cfg->advertisement_key, addr);

    /* Not latched on failure: the next pass retries. */
    if (od_hal_adv2_program(OD_HAL_ADV2_SET_NET_G, adv, (uint8_t)sizeof adv, addr,
                            OD_NET_G_ADV_INTERVAL_SLOTS) != OD_HAL_ADV_OK
        || od_hal_adv2_start(OD_HAL_ADV2_SET_NET_G) != OD_HAL_ADV_OK) {
        return;
    }
    s_started = true;
    od_log_info("net_g: advertising");
}
