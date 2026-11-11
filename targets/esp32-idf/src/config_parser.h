#ifndef CONFIG_PARSER_H
#define CONFIG_PARSER_H

#include <stdint.h>

#include "od_config_asm.h"   // OD_CONFIG_MAX_SIZE, defined there and overridden per target by shared/profiles.cmake

#define CONFIG_FILE_PATH "/config.bin"

// Chunked CONFIG_WRITE reassembly state moved to shared/core/od_config_asm.h (F3, 2026-08-05).
// The local chunked_write_state_t is GONE: its shape checks and bounds were the defect, and a
// per-target copy of the state is what let the bound and the buffer it guards be sized in two
// different files. The object now lives in communication.cpp as `g_configAsm`.

/**
 * Clear the chunked config-upload state.
 *
 * The single primitive for it. The three sites in communication.cpp that used to
 * clear it inline each zeroed a different subset -- one set only `active`, another
 * also the counters, none the totals -- so a teardown routed through the wrong one
 * left a partially-live upload. abortToKnownState() calls this too, which is what
 * gives session teardown any coverage of this state at all: it previously had no
 * reset function, so no disconnect path and no watchdog touched it.
 *
 * The payload buffer is deliberately not zeroed: `active = false` makes it
 * unreachable, and OD_CONFIG_MAX_SIZE is large enough that clearing it on every
 * teardown would be pointless work.
 */
void resetChunkedWriteState(void);

bool initConfigStorage();
void formatConfigStorage();
bool saveConfig(uint8_t* configData, uint32_t len);
bool clearStoredConfig(void);
bool loadConfig(uint8_t* configData, uint32_t* len);
bool hasValidStoredConfig(void);
bool loadGlobalConfig();
void full_config_init();
/* The SEC2 security boot (od_security_boot), once per boot after full_config_init(). */
void securityBoot(void);
/* od_claim_app_config_changed(): the policy rewrote the stored config; reload at the next
 * serviceSecurityPolicyReload() (loop task). */
void configChangedBySecurityPolicy(void);
void serviceSecurityPolicyReload(void);
/* The record workspace saveConfig() uses, lent to the SEC2 policy. */
uint8_t* getConfigWorkspace(uint32_t* cap);

// The one 4 KB config staging buffer, shared by every consumer that needs a
// whole config blob in RAM (loadGlobalConfig, handleReadConfig,
// hasValidStoredConfig). Safe to share because all config paths run
// synchronously on the loop task and none of them nest: no two callers ever
// hold it live at the same time. Callers must treat the contents as valid only
// until they return. Do NOT use this for chunkedWriteState -- that buffer stays
// live across BLE commands while other config work can run.
uint8_t* getConfigScratch(void);

#endif
