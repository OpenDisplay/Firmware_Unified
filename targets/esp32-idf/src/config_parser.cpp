#include "config_parser.h"
#include "od_config_carry.h"
#include "od_controller_esp.h"
#include "factory_config.h"
#include "structs.h"
#include "od_log.h"
#include "encryption_state.h"
#include "encryption.h"   // checkResetPin()
#include "od_session.h"
#include "od_session_app.h"
#include "power_latch.h"
#include "protocol_pending.h"  // OD_PANEL_IC_M5PAPER_960X540
#include "wifi_service.h"  // OPENDISPLAY_HAS_WIFI + lanActivePort()/lanTlsEnabled()

#include <stdio.h>
#include <string.h>

/* ESP32: config lives in NVS. Shared core owns the record framing, bounds, CRC and outcomes;
 * this file supplies the workspace and applies target-specific runtime state. */
#include "od_config_store.h"
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
#include "esp_netif.h"
#endif
#ifndef DEVICE_FLAG_PWR_PIN
#define DEVICE_FLAG_PWR_PIN (1 << 0)
#define DEVICE_FLAG_XIAOINIT (1 << 1)
#define DEVICE_FLAG_WS_PP_INIT (1 << 2)
#define DEVICE_FLAG_BATTERY_LATCH (1 << 3)
#define DEVICE_FLAG_PWR_LATCH_DFF (1 << 4)
#define DEVICE_FLAG_NM420_INIT (1 << 5)
#endif
void nm420_init();
void m5paper_init();

extern struct od_config globalConfig;
extern uint8_t activeLedInstance;
extern char wifiSsid[33];
extern char wifiPassword[33];
extern uint8_t wifiEncryptionType;
extern bool wifiConfigured;
extern char wifiServerUrl[65];
extern uint16_t wifiServerPort;
// extern bool wifiServerConfigured;  // dead -- see the 0x26 wifi_config parse
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
extern bool wifiConnected;
extern bool wifiInitialized;
#endif

void xiaoinit();
void powerDownExternalFlashFromConfig(void);
void ws_pp_init();
extern bool encryptionInitialized;

// Defined in main.h (the single-inclusion globals header), so it needs an extern
// here rather than an include -- main.h may not be included twice.
#include "od_config_asm.h"
#include "od_config_tlv.h"
extern struct od_config_asm g_configAsm;

void resetChunkedWriteState(void) {
    /* One line now: the state it clears is shared/core's. Kept as a named primitive because
     * abortToKnownState() and session_guard.cpp call it, and routing every teardown through one
     * function is what stopped three call sites each zeroing a different subset. */
    od_config_asm_reset(&g_configAsm);
}

bool initConfigStorage(){
    return od_config_store_init() == OD_CONFIG_STORE_OK;
}

void formatConfigStorage(){
    (void)od_config_store_clear();
}

// See getConfigScratch() in config_parser.h for the sharing contract. Replaces a
// per-consumer 4 KB buffer each in loadGlobalConfig (static), hasValidStoredConfig
// (static) and handleReadConfig (stack -- a 4 KB spike on the loop task).
static uint8_t configScratch[OD_CONFIG_MAX_SIZE];

uint8_t* getConfigScratch(void) {
    return configScratch;
}

bool saveConfig(uint8_t* configData, uint32_t len){
    /* The workspace od_config_store fills: header at offset 0, payload after it, written to the
     * medium as one span. Static because it is 4 KB and config writes are serialised on the
     * loop task. The factory-provisioning caller passes a flash pointer, which the core copies
     * in here -- there is nowhere else it could be assembled contiguously. */
    static uint8_t workspace[OD_CONFIG_STORE_MAX_RECORD];
    uint8_t carried = 0;

    /* A config without 0x80+ packets keeps the stored drive packets (od_config_carry.h). */
    const bool ok = od_config_store_save_carry(workspace, sizeof(workspace), configData, len,
                                               &carried) == OD_CONFIG_STORE_OK;
    if (ok && carried != 0u) {
        od_log_info("config: kept %u stored drive packet(s) the write did not carry",
                    (unsigned)carried);
    }
    return ok;
}

bool clearStoredConfig(void) {
    if (od_config_store_clear() != OD_CONFIG_STORE_OK) {
        return false;
    }
    /* One memset, not two: securityConfig is a member of globalConfig now, so zeroing the
     * config zeroes the key as well. */
    od_config_reset(&globalConfig);
    wifiConfigured = false;
    wifiSsid[0] = '\0';
    wifiPassword[0] = '\0';
    wifiEncryptionType = 0;
    return true;
}

bool loadConfig(uint8_t* configData, uint32_t* len){
    return od_config_store_load(configData, len) == OD_CONFIG_STORE_OK;
}

bool hasValidStoredConfig(void) {
    /* No presence probe: "valid" here means the magic and the CRC hold, which only a full
     * load can answer, and loadConfig() already returns false when nothing is stored. */
    uint32_t len = OD_CONFIG_MAX_SIZE;
    return loadConfig(getConfigScratch(), &len);
}

/* Two checksums used to live here and neither does now: the toolbox CRC-16/CCITT moved to
 * shared/core/od_config_tlv, and the storage record's CRC-32 to shared/core/od_config_store.
 * A local copy of a promoted function is dead code that still compiles, and the next edit to
 * one of them is the drift this repo exists to prevent. */

/* The WiFi side effects. Deliberately NOT promoted (shared/core/od_config.h): the credential
 * copies and the server_host numeric-IP coercion are LAN-transport behaviour on the one target
 * that has a LAN transport, not config parsing. od_config stores the 0x26 packet verbatim; this
 * turns it into what the listener needs, and runs once per load rather than once per packet.
 */
static void applyWifiConfig(const struct WifiConfig &wc) {
    memcpy(wifiSsid, wc.ssid, sizeof(wc.ssid));
    wifiSsid[32] = '\0';
    uint8_t ssidLen = 0;
    while (ssidLen < 32 && wifiSsid[ssidLen] != '\0') ssidLen++;

    memcpy(wifiPassword, wc.password, sizeof(wc.password));
    wifiPassword[32] = '\0';
    uint8_t passwordLen = 0;
    while (passwordLen < 32 && wifiPassword[passwordLen] != '\0') passwordLen++;

    wifiEncryptionType = wc.encryption_type;

    memcpy(wifiServerUrl, wc.server_host, 64);
    wifiServerUrl[64] = '\0';

    bool isStringFormat = false;
    for (int i = 0; i < 64; i++) {
        if (wifiServerUrl[i] == '\0') {
            isStringFormat = true;
            break;
        }
        if (i > 0 && wifiServerUrl[i] < 32 && wifiServerUrl[i] != '\0') {
            break;
        }
    }

    if (!isStringFormat && wifiServerUrl[4] == '\0' &&
        (wifiServerUrl[0] != 0 || wifiServerUrl[1] != 0 ||
         wifiServerUrl[2] != 0 || wifiServerUrl[3] != 0)) {
        uint8_t ip[4];
        ip[0] = wc.server_host[0];
        ip[1] = wc.server_host[1];
        ip[2] = wc.server_host[2];
        ip[3] = wc.server_host[3];
        snprintf(wifiServerUrl, 65, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        od_log_debug("Converted numeric IP to string: \"%s\"", wifiServerUrl);
    } else if (!isStringFormat && wifiServerUrl[0] != '\0') {
        uint32_t ipNum = (uint32_t)wc.server_host[0] |
                        ((uint32_t)wc.server_host[1] << 8) |
                        ((uint32_t)wc.server_host[2] << 16) |
                        ((uint32_t)wc.server_host[3] << 24);
        uint8_t ip[4];
        ip[0] = (ipNum >> 24) & 0xFF;
        ip[1] = (ipNum >> 16) & 0xFF;
        ip[2] = (ipNum >> 8) & 0xFF;
        ip[3] = ipNum & 0xFF;
        snprintf(wifiServerUrl, 65, "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        od_log_debug("Converted 32-bit integer to IP string: \"%s\"", wifiServerUrl);
    }

    // server_port is the one BIG-ENDIAN field in WifiConfig; read it byte-wise
    // (former reserved[64]=MSB, reserved[65]=LSB).
    wifiServerPort = (uint16_t)(((uint16_t)((const uint8_t*)&wc.server_port)[0] << 8) |
                                ((const uint8_t*)&wc.server_port)[1]);
    if (wifiServerPort == 0) {
        wifiServerPort = 2446;
    }

    // wifiServerConfigured is dead: it was only ever read by the log
    // lines that used to sit here. It described the old "tag pushes to
    // an upload server" model, but the LAN transport inverted that --
    // the device listens and the host connects to it, so server_host
    // gates nothing. server_host stays part of the 0x26 wire format.
    //
    // Report the endpoint the LAN listener will actually bind, not just
    // the raw config field: the TLS-PSK channel runs on server_port + 1
    // and there is no config entry for it.
#ifdef OPENDISPLAY_HAS_WIFI
    od_log_debug("LAN: %s on port %u (server_port %u)",
                 lanTlsEnabled() ? "TLS-PSK" : "plaintext",
                 (unsigned)lanActivePort(), (unsigned)wifiServerPort);
#else
    od_log_debug("LAN: transport not compiled in (server_port %u)", (unsigned)wifiServerPort);
#endif
    wifiConfigured = true;
    od_log_info("=== WiFi Configuration Loaded ===");
    // Do NOT log the SSID or password (credentials). Presence/length only.
    od_log_debug("SSID: (set, %u chars)", ssidLen);
    od_log_debug("Password: %s", passwordLen > 0 ? "(set)" : "(empty)");
    const char* encTypeStr = "Unknown";
    switch (wifiEncryptionType) {
        case 0x00: encTypeStr = "None (Open)"; break;
        case 0x01: encTypeStr = "WEP"; break;
        case 0x02: encTypeStr = "WPA"; break;
        case 0x03: encTypeStr = "WPA2"; break;
        case 0x04: encTypeStr = "WPA3"; break;
    }
    od_log_debug("Encryption Type: 0x%02X (%s)", wifiEncryptionType, encTypeStr);
    od_log_debug("SSID length: %u bytes", ssidLen);
    od_log_debug("Password length: %u bytes", passwordLen);
    od_log_debug("WiFi configured: true");
}

static void logWifiRuntimeState() {
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_DEBUG
    if ((globalConfig.system_config.communication_modes & OD_COMM_MODE_WIFI) == 0u) {
        return;
    }
    if (!wifiConfigured) {
        od_log_debug("WiFi Status: Configured but not loaded");
        return;
    }
    if (!wifiInitialized) {
        od_log_debug("WiFi Status: Not initialized");
        return;
    }
    if (!wifiConnected) {
        od_log_debug("WiFi Status: Disconnected");
        return;
    }

    char ipStr[16] = "?";
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ipInfo;
    if (sta != nullptr && esp_netif_get_ip_info(sta, &ipInfo) == ESP_OK) {
        snprintf(ipStr, sizeof(ipStr), IPSTR, IP2STR(&ipInfo.ip));
    }
    od_log_debug("WiFi Status: Connected (IP: %s)", ipStr);
#endif
}

/* THE PER-PACKET SWITCH IS GONE, and that is the promotion. It spelled out the instance caps
 * eight times, the DataExtended terminators once more, and the zero-key rule once -- each an
 * independent chance to compare against the wrong bound or normalise nothing. Storage now
 * happens once in shared/core/od_config.c, against the same aggregate every target keeps, and
 * what is left here is this target's own: the LED re-detect, the WiFi apply, and target-specific
 * configuration/runtime detail. Shared parsing reports its own outcomes directly.
 */
bool loadGlobalConfig(){
    wifiConfigured = false;
    wifiSsid[0] = '\0';
    wifiPassword[0] = '\0';
    wifiEncryptionType = 0;

    uint8_t* configData = getConfigScratch();
    uint32_t configLen = OD_CONFIG_MAX_SIZE;
    if (!loadConfig(configData, &configLen)) {
        od_config_reset(&globalConfig);
        od_controller_esp_reload();
        return false;
    }

    /* Resets, walks, stores and computes the advisory CRC. globalConfig.loaded is set only on a
     * clean walk; a blob that truncates half-way keeps the packets that preceded the truncation,
     * so `loaded` is what consumers must read, not the counts. */
    const enum od_config_tlv_result walk =
        od_config_parse(&globalConfig, od_span_make(configData, configLen), nullptr);
    od_controller_esp_reload();
    if (walk != OD_CFG_TLV_OK) return false;

    if (globalConfig.led_count > 0) {
        // Re-detect RGB LEDs after a config change.
        activeLedInstance = 0xFF;
    }
    if (globalConfig.wifi_config_loaded) {
        applyWifiConfig(globalConfig.wifi_config);
    }
    return true;
}

void full_config_init() {
    if (!initConfigStorage()) {
        return;
    }

#ifdef FACTORY_CLEAR_CONFIG_ON_BOOT
    od_log_info("Factory clear build: erasing stored config");
    clearStoredConfig();
    od_log_info("Config cleared; skipping load");
    return;
#endif

    bool configLoaded = loadGlobalConfig();
    if (!configLoaded && tryProvisionFactoryEmbed()) {
        configLoaded = loadGlobalConfig();
    }
    if (configLoaded) {
        logWifiRuntimeState();
        od_session_clear(od_session_app_state());
        encryptionInitialized = true;
        checkResetPin();
        if (globalConfig.loaded && (globalConfig.system_config.device_flags & DEVICE_FLAG_WS_PP_INIT)) {
            od_log_info("Device flag DEVICE_FLAG_WS_PP_INIT is set, calling ws_pp_init()...");
            ws_pp_init();
            od_log_info("ws_pp_init() completed");
        }
        if (globalConfig.loaded && (globalConfig.system_config.device_flags & DEVICE_FLAG_NM420_INIT)) {
            nm420_init();
        }
        if (globalConfig.loaded && globalConfig.display_count > 0 &&
            globalConfig.displays[0].panel_ic_type == OD_PANEL_IC_M5PAPER_960X540) {
            m5paper_init();
        }
        // Must run after config load: latch pins/flag come from globalConfig.
        powerLatchBegin();
    }
}
