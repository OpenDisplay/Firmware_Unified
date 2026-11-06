/* od_net_a -- network A (legacy offline-finding) advertising policy.
 *
 * The parts that are a NETWORK SPECIFICATION live here: which config makes the network active,
 * how the advertised address is derived from the key, and the 31-byte record layout. None of
 * that varies by BLE stack, so none of it belongs in a target. The stack calls sit behind
 * od_hal_adv2.
 *
 * THE KEY IS STATIC. FindMyConfig carries one 28-byte advertisement key and no rotation
 * schedule for this network -- net_a_time_base is documented as unused and written zero. So
 * the address and payload are computed once from config and change only when config does.
 * A rotating variant would be a different policy, not a parameter on this one.
 *
 * WHAT IS ADVERTISED IS A PUBLIC KEY, not a secret: FindMyConfig.net_a_adv_key is the
 * advertisement public key X. Nothing here handles private key material.
 */
#ifndef OD_NET_A_H
#define OD_NET_A_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OD_NET_A_ADV_KEY_LEN        28u
#define OD_NET_A_LEGACY_ADV_LEN     31u

/* 1600 * 0.625 ms = 1 s. The network's cadence, not a power-saving choice. */
#define OD_NET_A_ADV_INTERVAL_SLOTS 1600u

/* Is network A configured and enabled?
 *
 * Requires BOTH the enable flag and a non-zero key. A zeroed key with the flag set is the
 * shape an unprovisioned-but-enabled device has, and advertising it would publish a valid
 * record for the all-zero identity. */
bool od_net_a_config_active(const struct FindMyConfig *cfg);

/* Derive the advertised BLE address from the key. `addr` is little-endian: addr[5] is the
 * most significant byte and carries the static-random bits. */
void od_net_a_derive_address(const uint8_t key[OD_NET_A_ADV_KEY_LEN], uint8_t addr[6]);

/* Build the 31-byte advertisement. Returns the length written, or 0 if `cap` is too small. */
uint8_t od_net_a_build_adv(const uint8_t key[OD_NET_A_ADV_KEY_LEN],
                           uint8_t *adv, uint8_t cap);

/* Bring advertising into line with `cfg`: start it when the config is active and it is not
 * already running, stop it when it is not. Idempotent and cheap to call every pass.
 *
 * Does nothing when the build has no second advertising set. */
void od_net_a_sync(const struct FindMyConfig *cfg);

/* Forget that advertising was started, so the next sync reprograms from scratch. Call after a
 * config write: the key may have changed, and the address is derived from it. */
void od_net_a_invalidate(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_NET_A_H */
