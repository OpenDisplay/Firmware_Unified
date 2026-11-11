#ifndef OPENDISPLAY_CONFIG_STORAGE_H
#define OPENDISPLAY_CONFIG_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config_asm.h" /* OD_CONFIG_MAX_SIZE, defined there and overridden per target by shared/profiles.cmake */

/*
 * The stored record is [magic4][version4][crc4][data_len4][data[len]] = 16 + len
 * bytes, saved as a single Zephyr settings/NVS item. The nRF54L RRAM NVS sector
 * is 4096 B, so the largest storable record is ~sector - 4*ATE = 4064 B. The BLE
 * write paths cap an inbound config at MAX_CONFIG_CHUNKS(20)*CONFIG_CHUNK_SIZE(200)
 * = 4000 B (chunked) or 200 B (single-shot), so any client-writable config fits
 * (16 + 4000 = 4016 B < 4064 B). A blob larger than that is unreachable over BLE
 * and, if ever provisioned, fails cleanly via settings_save_one (see saveConfig).
 */

bool initConfigStorage(void);

bool saveConfig(uint8_t *config_data, uint32_t len);

bool loadConfig(uint8_t *config_data, uint32_t *len);

bool clearStoredConfig(void);

/* The record workspace saveConfig uses, lent to the SEC2 policy (od_claim_app_workspace). */
uint8_t *opendisplay_config_workspace(uint32_t *cap);

#endif
