/* od_cfg_policy.h -- the SEC2 rules for what may be stored in, and read from, the config record
 * (SECURITY_V2 sections 10.1-10.4, 10.6; FACTORY_RESET.md R1b, R5c).
 *
 * EVERY STORE GOES THROUGH HERE. Each target's one save funnel (saveConfig) calls
 * od_cfg_policy_save() and its one clear funnel (clearStoredConfig) od_cfg_policy_clear_store(),
 * so CONFIG_WRITE, CONFIG_CHUNK (last chunk), CONFIG_CLEAR, the LAN write and factory-embedded
 * provisioning all meet the same rules. od_config_store itself stays policy-free: the policy's own
 * rewrites (0x27 at rest, scrub, the CONFIG_CLEAR replacement) and the factory-reset erase call it
 * directly.
 *
 * ONE WALK (A13): the secret scan is od_config_tlv_walk(), the iterator od_config_parse() uses,
 * over the same assembled blob. A walk that does not end exactly at the end of the blob (a
 * truncated packet, an unknown legacy id that abandons the rest) refuses the write with
 * OD_ERR_CONFIG_WRITE_FAILED on every device.
 *
 * THE CLOSED SECRET LIST (section 10.4): 0x26 password[32]; 0x27 encryption_enabled /
 * encryption_key[16]; 0x2D eik[32], net_a_adv_key[28], advertisement_key[20]. Every instance.
 *
 * OD_SEC2=0 (a "Core" build without the module, Q14): every blob carrying a secret is refused
 * with 0x10, whatever the device state. There is no flag that allows storing one.
 *
 * CONTEXT: main loop only.
 */
#ifndef OD_CFG_POLICY_H
#define OD_CFG_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config_store.h"
#include "od_span.h"
#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef OD_SEC2
#define OD_SEC2 1
#endif

#define OD_CFG_POLICY_OK 0xFFu   /* admitted; anything else is the 4g NACK byte */

/* Admit, and transform in place, one complete config blob about to be stored.
 * blob[0 .. *len) is the blob, `cap` the bytes available at blob (insertion grows *len by 66).
 * Returns OD_CFG_POLICY_OK, or OD_ERR_CONFIG_WRITE_FAILED (0x00: unclean walk, no room),
 * OD_ERR_CONFIG_SECRET_NEEDS_CLAIM (0x10) or OD_ERR_CONFIG_KEY_MISMATCH (0x11). On a refusal
 * the blob may be partly transformed and must not be stored.
 *   claimed:   0x27 key 0 or K_dev (else 0x11); stored at rest (enabled 1, K_v1, marker 0x02);
 *              0x27 absent: the stored 0x27 at rest is inserted before the first packet whose id
 *              is > 0x27; an enabled reset pin is sticky (section 10.6); secrets accepted.
 *   unclaimed: any 0x27 key / enabled, or any secret, gets 0x10 -- except on a LEGACY_SECRETS
 *              device a secret byte-identical to the stored one (the read-modify-write rule).
 *   LOCKED:    0x10 for anything carrying a secret (a LOCKED device is gated anyway). */
uint8_t od_cfg_policy_admit(uint8_t *blob, uint32_t *len, uint32_t cap);

/* od_config_store_save() behind od_cfg_policy_admit(): the payload is moved into the workspace's
 * slot, admitted (and transformed) there, then stored. A refusal returns OD_CONFIG_STORE_IO with
 * nothing written, and od_config_store_save_error() then reports the policy's byte. */
enum od_config_store_result od_cfg_policy_save(void *workspace, uint32_t workspace_cap,
                                               const uint8_t *payload, uint32_t len);

/* CONFIG_CLEAR (10.6). Unclaimed: erase the record. Claimed: replace it with a config holding only
 * the stored 0x27 at rest. LOCKED: refused (IO), never erasing the claim evidence. */
enum od_config_store_result od_cfg_policy_clear_store(void);

/* The stored blob carries a WiFi password or FindMy key material (rows 5-7 of 10.1). Loads the
 * record into the workspace. false also when there is no record or it does not walk. */
bool od_cfg_policy_stored_has_secrets(void);

/* The stored 0x27 has encryption_enabled == 0 with a non-zero key (a 2.x key never in force):
 * zero it in place (one rewrite; 10.1 note). Returns true when it rewrote. */
bool od_cfg_policy_zero_stale_key(void);

/* od_sec2_app_scrub() / od_sec2_app_rest_sync() (od_sec2_app.h), forwarded by od_sec2_hooks.c. */
bool od_cfg_policy_scrub(void);
void od_cfg_policy_rest_sync(void);

/* Read view (10.2), applied by od_config_read to every chunk it copies: blob is the stored
 * record payload being read, [off, off + n) the bytes just copied into dst. Patches the 0x27
 * fields to the truthful values (claimed: enabled 1, K_dev, reserved[0] 0; else 0 / 0 / 0). */
void od_cfg_policy_read_view(const uint8_t *blob, uint32_t blob_len, uint32_t off,
                             uint8_t *dst, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* OD_CFG_POLICY_H */
