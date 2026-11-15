/* od_hal_adv2 on EFR32BG22 (BGAPI) -- one advertising set per finding network (A and G).
 *
 * Each set is created lazily on its first program() rather than at boot, so a device whose
 * config never enables a finding network never allocates one. BGAPI advertising sets are a fixed
 * pool (SL_BT_CONFIG_USER_ADVERTISERS); spending one unconditionally would take it from a
 * feature that does use it.
 */
#include "od_hal_adv2.h"

#include "od_log.h"
#include "sl_bt_api.h"

#define OD_ADV2_NO_SET 0xFFu

static uint8_t s_set[OD_HAL_ADV2_SETS] = { OD_ADV2_NO_SET, OD_ADV2_NO_SET };
static bool    s_active[OD_HAL_ADV2_SETS];

bool od_hal_adv2_supported(void)
{
    return true;
}

static enum od_hal_adv_result from_sc(sl_status_t sc)
{
    if (sc == SL_STATUS_OK) {
        return OD_HAL_ADV_OK;
    }
    /* Out of advertising sets or momentarily busy is backpressure, not a hard fault: the
     * caller leaves its state alone and tries again next pass. */
    if (sc == SL_STATUS_NO_MORE_RESOURCE || sc == SL_STATUS_BUSY) {
        return OD_HAL_ADV_RETRY;
    }
    return OD_HAL_ADV_ERROR;
}

static enum od_hal_adv_result ensure_set(uint8_t set)
{
    if (s_set[set] != OD_ADV2_NO_SET) {
        return OD_HAL_ADV_OK;
    }
    uint8_t handle = OD_ADV2_NO_SET;
    sl_status_t sc = sl_bt_advertiser_create_set(&handle);
    if (sc != SL_STATUS_OK) {
        if (sc == SL_STATUS_NO_MORE_RESOURCE) {
            /* NOT backpressure, however much it looks like it from the status code: the
             * advertising-set pool is sized at compile time, so this fails identically on
             * every pass forever. Reporting it as RETRY is what makes a device run normally
             * and simply never be findable, with nothing to search for. Name the cause and
             * say it ONCE -- the caller retries every pass and would otherwise flood. */
            static bool told;
            if (!told) {
                told = true;
                od_log_error("adv2: no advertising set free -- raise "
                             "SL_BT_CONFIG_USER_ADVERTISERS (needs >= 3)");
            }
            return OD_HAL_ADV_ERROR;
        }
        od_log_warn("adv2: create_set sc=0x%04X", (unsigned)sc);
        return from_sc(sc);
    }
    s_set[set] = handle;
    return OD_HAL_ADV_OK;
}

enum od_hal_adv_result od_hal_adv2_program(uint8_t set, const uint8_t *adv, uint8_t adv_len,
                                           const uint8_t addr[6],
                                           uint16_t interval_slots)
{
    if (set >= OD_HAL_ADV2_SETS || adv == NULL || adv_len == 0u || addr == NULL) {
        return OD_HAL_ADV_ERROR;
    }

    enum od_hal_adv_result r = ensure_set(set);
    if (r != OD_HAL_ADV_OK) {
        return r;
    }

    /* THIS SET ONLY. sl_bt_advertiser_set_random_address takes the set handle, so the device
     * identity address and the other sets are untouched -- which is the whole reason the
     * finding networks can run alongside normal OpenDisplay advertising. */
    bd_addr want;
    bd_addr applied;
    for (unsigned i = 0u; i < 6u; i++) {
        want.addr[i] = addr[i];
    }
    sl_status_t sc = sl_bt_advertiser_set_random_address(s_set[set], sl_bt_gap_static_address,
                                                         want, &applied);
    if (sc != SL_STATUS_OK) {
        od_log_warn("adv2: set_random_address sc=0x%04X", (unsigned)sc);
        return from_sc(sc);
    }

    sc = sl_bt_advertiser_set_timing(s_set[set], interval_slots, interval_slots, 0, 0);
    if (sc != SL_STATUS_OK) {
        od_log_warn("adv2: set_timing sc=0x%04X", (unsigned)sc);
        return from_sc(sc);
    }

    /* Installed verbatim: these bytes are a network specification. */
    sc = sl_bt_legacy_advertiser_set_data(s_set[set], sl_bt_advertiser_advertising_data_packet,
                                          adv_len, adv);
    if (sc != SL_STATUS_OK) {
        od_log_warn("adv2: set_data sc=0x%04X", (unsigned)sc);
        return from_sc(sc);
    }
    return OD_HAL_ADV_OK;
}

enum od_hal_adv_result od_hal_adv2_start(uint8_t set)
{
    if (set >= OD_HAL_ADV2_SETS || s_set[set] == OD_ADV2_NO_SET) {
        return OD_HAL_ADV_ERROR;      /* program() was never called */
    }
    if (s_active[set]) {
        return OD_HAL_ADV_ALREADY;
    }
    sl_status_t sc = sl_bt_legacy_advertiser_start(s_set[set],
                                                   sl_bt_legacy_advertiser_non_connectable);
    if (sc != SL_STATUS_OK) {
        od_log_warn("adv2: start sc=0x%04X", (unsigned)sc);
        return from_sc(sc);
    }
    s_active[set] = true;
    return OD_HAL_ADV_OK;
}

enum od_hal_adv_result od_hal_adv2_stop(uint8_t set)
{
    if (set >= OD_HAL_ADV2_SETS || s_set[set] == OD_ADV2_NO_SET || !s_active[set]) {
        return OD_HAL_ADV_NOT_ACTIVE;
    }
    sl_status_t sc = sl_bt_advertiser_stop(s_set[set]);
    s_active[set] = false;     /* cleared regardless: a stop that errored still left it not ours */
    if (sc != SL_STATUS_OK) {
        od_log_warn("adv2: stop sc=0x%04X", (unsigned)sc);
        return from_sc(sc);
    }
    return OD_HAL_ADV_OK;
}
