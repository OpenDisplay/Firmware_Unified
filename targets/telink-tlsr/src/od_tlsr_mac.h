/* The tag's BLE address, the same one the legacy firmware uses, so a tag keeps its address (and the
 * low three bytes its name shows) across legacy -> OpenDisplay -> legacy.
 *
 * The legacy firmware derives it from the flash chip's 16-byte unique ID (SPI command 0x4B): id[i] =
 * uid[i] ^ uid[i + 8] for i < 8, and the address is id[0..5], least significant byte first,
 * with no type bits forced (one tag here is 34:64:60:42:D2:41). Its name is a fixed prefix plus the hex of
 * id[2], id[1], id[0]. Source: the v108 image (2025-09-18_180bec5292.bin, function at 0xd7d4,
 * which logs "Reading UID"); its TLSR source is not public.
 *
 * Order: the UID; then the SDK's MAC record at OD_TLSR_MAC_ADDR, blank on every legacy tag seen,
 * and only ever read; then a random address in Telink's vendor block. The random one is made
 * once and kept (od_tlsr_mac_app_load/save, the secrets record in od_tlsr_store.c), because the
 * address is also the SEC2 device id: one that changed at every boot would strand a claimed
 * tag. Include after od_tlsr_sdk.h. */
#ifndef OD_TLSR_MAC_H
#define OD_TLSR_MAC_H

#include "od_tlsr_flash.h"
#include "od_tlsr_layout.h"

#include <stdbool.h>
#include <string.h>

#define OD_TLSR_FLASH_CMD_READ_UID 0x4Bu

/* Target hooks for the random fallback. load: true and the kept address when one is stored.
 * save: best effort; if it fails the address holds for this boot only. */
bool od_tlsr_mac_app_load(unsigned char mac[6]);
void od_tlsr_mac_app_save(const unsigned char mac[6]);

/* The SDK's "no UID" answer (flash_common.c), and the two a read that returned nothing gives. */
static inline int od_tlsr_uid_valid(const unsigned char uid[16])
{
    unsigned int i, sdk_none = 0u, ones = 0u, zeros = 0u;

    for (i = 0u; i < 16u; ++i) {
        sdk_none += uid[i] == ((i & 1u) ? 0x01u : 0x51u);
        ones += uid[i] == 0xFFu;
        zeros += uid[i] == 0x00u;
    }
    return sdk_none != 16u && ones != 16u && zeros != 16u;
}

static inline void od_tlsr_mac_from_uid(const unsigned char uid[16], unsigned char mac[6])
{
    unsigned int i;

    for (i = 0u; i < 6u; ++i) {
        mac[i] = (unsigned char)(uid[i] ^ uid[i + 8u]);
    }
}

static inline void od_tlsr_mac_read(unsigned char mac[6])
{
    static const unsigned char blank[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    unsigned char uid[16], again[16];

    flash_read_uid(OD_TLSR_FLASH_CMD_READ_UID, uid);
    flash_read_uid(OD_TLSR_FLASH_CMD_READ_UID, again);
    if (memcmp(uid, again, sizeof uid) == 0 && od_tlsr_uid_valid(uid)) {
        od_tlsr_mac_from_uid(uid, mac);
        return;
    }
    od_tlsr_flash_read(OD_TLSR_MAC_ADDR, mac, 6u);
    if (memcmp(mac, blank, 6u) != 0) {
        return;
    }
    if (od_tlsr_mac_app_load(mac)) {
        return;
    }
    generateRandomNum(3, mac);
    mac[3] = 0x38u;
    mac[4] = 0xC1u;
    mac[5] = 0xA4u;
    od_tlsr_mac_app_save(mac);
}

#endif
