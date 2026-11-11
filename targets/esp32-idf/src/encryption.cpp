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

/* ESP32: config storage is NVS, not LittleFS -- see config_parser.cpp and
 * shared/hal/od_hal_nvs.h. This file only needs to invalidate the stored record. */
#include "od_hal_nvs_secure.h"
#include <esp_system.h>


/* Same split for the two other Arduino primitives this file uses, both in checkResetPin(). */
static inline void od_delay_ms(uint32_t ms) {
    od_hal_delay_ms(ms);
}

static inline int od_read_pin(uint8_t pin) {
    return od_hal_gpio_read(pin);
}

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

/* What remains here is the DEVICE IDENTITY and the local key-loss controls: the efuse-derived
 * auth id and advertised name, the secure erase of the stored config, and the reset pin. The
 * session itself -- handshake, KDF, records -- is shared/core/od_sec2.c. */

static constexpr const char* CONFIG_FILE_PATH_LOCAL = "/config.bin";

void reboot();

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

void secureEraseConfig() {
    od_log_info("=== SECURE ERASE CONFIG ===");

    /* Was: open the stored config file, zero its bytes, then remove it. The NVS port briefly
     * reduced this to a plain od_hal_nvs_erase(), which drops the overwrite entirely --
     * nvs_erase_key() only marks the entry deleted, so the AES-128 master key in config
     * packet 0x27 stayed recoverable from a raw flash dump after a "secure erase".
     *
     * od_hal_nvs_secure_erase() restores the zero-write. Read its contract in od_hal_nvs_secure.h
     * before relying on it: on a log-structured store it is best-effort, not a guaranteed
     * overwrite. The overwrite stays inside the HAL rather than here, so this file is not a
     * second writer of the config record's byte format. */
    (void)od_hal_nvs_secure_erase();
    od_log_info("Config securely erased");
}

void checkResetPin() {
    if (!(securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED)) {
        return;
    }

    uint8_t pin = securityConfig.reset_pin;
    bool polarity = (securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_POLARITY) != 0;
    bool pullup = (securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_PULLUP) != 0;
    bool pulldown = (securityConfig.flags & OD_SECURITY_FLAG_RESET_PIN_PULLDOWN) != 0;

    od_log_debug("Checking reset pin %u (polarity: %s, pullup: %d, pulldown: %d)",
                 pin, polarity ? "HIGH" : "LOW", pullup, pulldown);

    /* One call where Arduino needed two: pinMode(INPUT) then a conditional re-pinMode with the
     * pull. Asking for neither pull IS the plain-INPUT case, so the intermediate configuration
     * disappears and the pad ends in the same state. */
    od_hal_gpio_config_input(pin, pullup, pulldown);

    od_delay_ms(100);
    bool pinState = (od_read_pin(pin) != 0);

    if (pinState == polarity) {
        od_log_warn("Reset pin triggered! Securely erasing config and rebooting...");
        secureEraseConfig();
        od_delay_ms(100);
        reboot();
    } else {
        od_log_debug("Reset pin not triggered (state: %s)", pinState ? "HIGH" : "LOW");
    }
}
