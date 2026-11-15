/* od_hal_adv2.h -- OPTIONAL extra advertising sets, one per offline-finding network.
 *
 * WHY THIS IS NOT od_hal_adv. That HAL installs one COMPLETE, IMMUTABLE 16-byte MSD payload on
 * the device's own identity address, and its contract says the target owns AD-record packing
 * while the shared controller owns timing. None of that fits a finding network: the payload is
 * a 31-byte record the network defines byte-for-byte, the address is DERIVED FROM THE KEY and
 * is not the device's identity, and the interval is the network's to dictate. Widening
 * od_hal_adv to carry a set index and a variable payload would change a contract all three
 * targets already implement, for a feature one of them needs.
 *
 * So this is additive and declinable. A target with no second advertising set returns false
 * from od_hal_adv2_supported() and links nothing further -- no dummy entry points, matching
 * CLAUDE.md decision 9 (a target must not pay for a feature it lacks).
 *
 * INDEXED BY NETWORK. Each finding network owns one set, named by an OD_HAL_ADV2_SET_* index:
 * the networks run concurrently, on different derived addresses and payloads, so they cannot
 * share a set. The index is the network, not a stack handle -- the target maps it.
 *
 * THE PAYLOAD IS OPAQUE TO THE TARGET. Unlike od_hal_adv, the target does NOT pack AD records
 * here -- it installs the bytes it is given. The record layout is a network specification, not
 * a stack preference, and a target that "helpfully" prepended flags would break it invisibly:
 * the advertisement still goes out and nothing ever reports an error, it is simply not findable.
 *
 * CONTEXT, as for od_hal_adv: called from the application loop, never a stack callback.
 */
#ifndef OD_HAL_ADV2_H
#define OD_HAL_ADV2_H

#include <stdbool.h>
#include <stdint.h>

#include "od_hal_adv.h"   /* enum od_hal_adv_result, shared with the primary set */

#ifdef __cplusplus
extern "C" {
#endif

#define OD_HAL_ADV2_SET_NET_A 0u   /* od_net_a: legacy offline finding */
#define OD_HAL_ADV2_SET_NET_G 1u   /* od_net_g: network G (FMDN) */
#define OD_HAL_ADV2_SETS      2u

/* Does this build have the finding-network advertising sets?
 *
 * Answer from the BUILD, not from runtime state: callers use it to decide whether to run the
 * finding-network policy at all, and a target that could answer "yes, later" would make that
 * policy flap. */
bool od_hal_adv2_supported(void);

/* Install the advertisement for set `set` (an OD_HAL_ADV2_SET_* index; anything else is
 * OD_HAL_ADV_ERROR).
 *
 * `adv` / `adv_len` are the complete AD-record bytes, installed verbatim.
 * `addr` is a 6-byte little-endian BLE address (addr[5] is the most significant byte, which is
 * where the static-random bits live) that the target must apply to THIS SET ONLY -- changing
 * the device identity address would take the primary set with it.
 * `interval_slots` is both min and max, in 0.625 ms units.
 *
 * Called only while that set is believed inactive, so no live-update case arises. */
enum od_hal_adv_result od_hal_adv2_program(uint8_t set, const uint8_t *adv, uint8_t adv_len,
                                           const uint8_t addr[6],
                                           uint16_t interval_slots);

/* Start/stop one set. Non-connectable: a finding advertisement that accepted
 * connections would expose the device's GATT on an address meant to be anonymous. */
enum od_hal_adv_result od_hal_adv2_start(uint8_t set);
enum od_hal_adv_result od_hal_adv2_stop(uint8_t set);

#ifdef __cplusplus
}
#endif
#endif /* OD_HAL_ADV2_H */
