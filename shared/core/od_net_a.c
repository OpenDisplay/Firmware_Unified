/* od_net_a.c -- see od_net_a.h. Network specification here, stack calls behind od_hal_adv2. */
#include "od_net_a.h"

#include <string.h>

#include "od_hal_adv2.h"
#include "od_log.h"

static bool s_started;

static bool key_nonzero(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        if (p[i] != 0u) {
            return true;
        }
    }
    return false;
}

bool od_net_a_config_active(const struct FindMyConfig *cfg)
{
    if (cfg == NULL) {
        return false;
    }
    if ((cfg->flags & OD_FINDMY_FLAG_NET_A_ENABLE) == 0u) {
        return false;
    }
    return key_nonzero(cfg->net_a_adv_key, OD_NET_A_ADV_KEY_LEN);
}

void od_net_a_derive_address(const uint8_t key[OD_NET_A_ADV_KEY_LEN], uint8_t addr[6])
{
    /* The top six bytes of the key become the address, most significant first, with the two
     * high bits of addr[5] forced to 0b11 to mark it static random. Those two displaced bits
     * are not lost: they are re-sent as the status byte at the end of the payload, which is
     * what lets a finder reconstruct the full key. */
    addr[5] = (uint8_t)(key[0] | 0xC0u);
    addr[4] = key[1];
    addr[3] = key[2];
    addr[2] = key[3];
    addr[1] = key[4];
    addr[0] = key[5];
}

uint8_t od_net_a_build_adv(const uint8_t key[OD_NET_A_ADV_KEY_LEN],
                           uint8_t *adv, uint8_t cap)
{
    if (adv == NULL || cap < OD_NET_A_LEGACY_ADV_LEN) {
        return 0u;
    }
    /* One AD structure, defined by the network and not negotiable:
     *   [0]      0x1E  length (30 bytes follow)
     *   [1]      0xFF  manufacturer specific data
     *   [2..3]   0x4C 0x00  Apple, little-endian company id
     *   [4]      0x12  offline finding type
     *   [5]      0x19  payload length (25)
     *   [6]      0x00  status
     *   [7..28]  key[6..27], the remaining 22 bytes of the public key
     *   [29]     key[0] >> 6, the two bits displaced by the static-random marker
     *   [30]     0x00  hint
     * The first six key bytes are not repeated here -- they are in the address. */
    adv[0] = 0x1Eu;
    adv[1] = 0xFFu;
    adv[2] = 0x4Cu;
    adv[3] = 0x00u;
    adv[4] = 0x12u;
    adv[5] = 0x19u;
    adv[6] = 0x00u;
    memcpy(&adv[7], &key[6], 22u);
    adv[29] = (uint8_t)(key[0] >> 6);
    adv[30] = 0x00u;
    return OD_NET_A_LEGACY_ADV_LEN;
}

void od_net_a_invalidate(void)
{
    s_started = false;
}

void od_net_a_sync(const struct FindMyConfig *cfg)
{
    uint8_t adv[OD_NET_A_LEGACY_ADV_LEN];
    uint8_t addr[6];
    uint8_t adv_len;

    if (!od_hal_adv2_supported()) {
        return;
    }

    if (!od_net_a_config_active(cfg)) {
        if (s_started) {
            (void)od_hal_adv2_stop();
            s_started = false;
            od_log_info("net_a: stopped (config inactive)");
        }
        return;
    }
    if (s_started) {
        return;
    }

    /* Stop before reprogramming. The address belongs to the set, and a stack may refuse to
     * change it underneath a running advertiser -- which would leave the OLD key advertising
     * while config says otherwise. NOT_ACTIVE here is the expected answer, not a fault. */
    (void)od_hal_adv2_stop();

    adv_len = od_net_a_build_adv(cfg->net_a_adv_key, adv, (uint8_t)sizeof adv);
    if (adv_len == 0u) {
        od_log_error("net_a: payload build failed");
        return;
    }
    od_net_a_derive_address(cfg->net_a_adv_key, addr);

    if (od_hal_adv2_program(adv, adv_len, addr, OD_NET_A_ADV_INTERVAL_SLOTS)
        != OD_HAL_ADV_OK) {
        /* Left unstarted deliberately: the next sync pass retries. Nothing latches, because a
         * finding network that stays dark after one transient stack error is worse than a
         * retry that occasionally logs. */
        return;
    }
    if (od_hal_adv2_start() != OD_HAL_ADV_OK) {
        return;
    }

    s_started = true;
    od_log_info("net_a: advertising, %u byte payload", (unsigned)adv_len);
}
