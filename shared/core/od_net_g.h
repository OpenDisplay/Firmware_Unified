/* od_net_g -- network G (Find My Device Network, "FMDN") advertising policy.
 *
 * The network-specification half, as od_net_a is for network A: when the network is active,
 * how the address derives from the key, and the record layout. Stack calls sit behind
 * od_hal_adv2 on set OD_HAL_ADV2_SET_NET_G.
 *
 * WIRE COMPATIBILITY WITH Firmware_Silabs 2.3.0 is the requirement, so a unit provisioned there
 * stays findable after the OTA with the same 0x2D packet. What 2.3.0 puts on air is:
 *
 *   SECP160R1 (fmdn_curve 0): a static frame built from FindMyConfig.advertisement_key (the host
 *     computes generate_eid(eik, 0)), legacy non-connectable, 1 s interval, on the static random
 *     address advertisement_key[0..5] with the top two bits forced to 0b11. This is the
 *     provisioning path the 0x2D contract documents.
 *
 *   SECP256R1 (fmdn_curve 1): NOTHING. 2.3.0 has code for an on-device rotating EID, but its
 *     PSA configuration has no SHA-256 (psa_hash_compute ends in the CRYPTOACC stub that returns
 *     PSA_ERROR_NOT_SUPPORTED), so every EID computation fails and the set never starts. No
 *     unit can therefore rely on it, and it is not active here either.
 *
 * THE HASHED-FLAGS BYTE IS 0x00. 2.3.0 means to send flags XOR the low byte of SHA-256(r), but
 * the same missing SHA-256 makes that function return 0 on every unit, so 0x00 is what the
 * fleet advertises. Reproduced exactly: the EID is what the network resolves, and a byte that
 * changed across the OTA is a difference nobody could observe from the device.
 *
 * WHAT IS ADVERTISED IS PUBLIC: advertisement_key is an EID, not the EIK. Nothing here reads
 * FindMyConfig.eik.
 */
#ifndef OD_NET_G_H
#define OD_NET_G_H

#include <stdbool.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OD_NET_G_EID_LEN          20u
#define OD_NET_G_LEGACY_ADV_LEN   29u

/* 1600 * 0.625 ms = 1 s, as 2.3.0. */
#define OD_NET_G_ADV_INTERVAL_SLOTS 1600u

/* FMDN frame type for the static (unwanted-tracking-protection) frame 2.3.0 sends. */
#define OD_NET_G_FRAME_TYPE_UTP   0x41u

/* Is network G configured, enabled, and on a curve this firmware advertises?
 * Requires the FMDN enable flag, fmdn_curve == SECP160R1 and a non-zero advertisement_key. */
bool od_net_g_config_active(const struct FindMyConfig *cfg);

/* Address from advertisement_key[0..5]: little-endian, addr[5] is the MSB and carries the
 * static-random marker. */
void od_net_g_derive_address(const uint8_t eid[OD_NET_G_EID_LEN], uint8_t addr[6]);

/* Build the 29-byte advertisement. Returns the length written, or 0 if `cap` is too small. */
uint8_t od_net_g_build_adv(const uint8_t eid[OD_NET_G_EID_LEN], uint8_t *adv, uint8_t cap);

/* Bring advertising into line with `cfg`; idempotent, cheap every pass. */
void od_net_g_sync(const struct FindMyConfig *cfg);

/* Forget that advertising was started. Call after a config write. */
void od_net_g_invalidate(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_NET_G_H */
