#include "encryption.h"
#include "od_span.h"
#include "communication.h"
#include "encryption_state.h"
#include "od_log.h"

#include "esp_mac.h"       // esp_efuse_mac_get_default -- was ESP.getEfuseMac() via the shim
#include "od_hal_crypto.h"
#include "od_hal_gpio.h"
#include "od_hal_sleep.h"
#include <stdio.h>
#include <string.h>

#include <esp_system.h>


void getAuthDeviceIdBytes(uint8_t* device_id) {
    if (device_id == nullptr) return;
    uint8_t macb[6] = {0};
    esp_efuse_mac_get_default(macb);
    /* ESP.getEfuseMac() returns the six factory bytes packed little-endian into a uint64_t,
     * i.e. byte 0 of the MAC in the LOW byte. `mac >> 16` therefore selected MAC bytes 2..5.
     * Reproduced exactly rather than "tidied": this feeds the AUTH device id, so a different
     * packing is a different device to the host. */
    uint64_t mac = ((uint64_t)macb[0])       | ((uint64_t)macb[1] << 8)  |
                   ((uint64_t)macb[2] << 16) | ((uint64_t)macb[3] << 24) |
                   ((uint64_t)macb[4] << 32) | ((uint64_t)macb[5] << 40);
    uint32_t id = (uint32_t)(mac >> 16);
    device_id[0] = (uint8_t)(id >> 24);
    device_id[1] = (uint8_t)(id >> 16);
    device_id[2] = (uint8_t)(id >> 8);
    device_id[3] = (uint8_t)(id);
}

/* What remains here is the DEVICE IDENTITY: the efuse-derived auth id and advertised name. The
 * session is shared/core/od_sec2.c; the reset pin and the factory reset are
 * shared/core/od_factory_reset.c (hooks in od_claim_app_esp.cpp). */

void getChipIdHex(char* out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    /* Refuse rather than truncate: a shortened device id is not a degraded id, it is a
     * DIFFERENT device to the host, which keys on the advertised name. */
    if (out_size < OD_CHIP_ID_HEX_LEN + 1u) {
        out[0] = '\0';
        return;
    }
    uint8_t macb[6] = {0};
    esp_efuse_mac_get_default(macb);
    /* Same little-endian packing as getAuthDeviceIdBytes() above -- see the note there. The
     * advertised device NAME is derived from this, and the fleet is keyed on it. */
    uint64_t macAddress = ((uint64_t)macb[0])       | ((uint64_t)macb[1] << 8)  |
                          ((uint64_t)macb[2] << 16) | ((uint64_t)macb[3] << 24) |
                          ((uint64_t)macb[4] << 32) | ((uint64_t)macb[5] << 40);
    uint32_t chipId = (uint32_t)(macAddress >> 24) & 0xFFFFFFu;
    snprintf(out, out_size, "%06X", (unsigned)chipId);
    od_log_debug("Chip ID: %06X", (unsigned)chipId);
    od_log_debug("Using chip ID: %s", out);
    return;
}

