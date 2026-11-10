/* od_config_carry -- keep a device's drive packets across a config write that lacks them.
 *
 * PROPOSAL, awaiting a maintainer decision (merge vs refuse). A 2.x client writes configs with
 * no 0x80+ packets at all; written as-is over a config-driven device's record, that erases the
 * 0xAE-0xB1 drive packets and leaves a panel that draws nothing until someone re-provisions it.
 * This wrapper merges instead: when the incoming config carries no 0x80+ packet and the stored
 * one has drive packets, they are appended (unchanged, in stored order) before the CRC tail.
 *
 * An incoming config carrying ANY 0x80+ packet is stored exactly as sent, so a 3.x client
 * owns the drive data. To clear it deliberately, a client sends a zero-length 0xB1
 * ([num][0xB1][00 00]): the index ignores empty bodies, so the device then has no drive data.
 *
 * Needs the APP_EPD tier (od_cfg_index) and HAL_NVS. The alternative a maintainer may prefer is
 * to refuse such a write; that is a one-branch change here.
 */
#ifndef OD_CONFIG_CARRY_H
#define OD_CONFIG_CARRY_H

#include <stdbool.h>
#include <stdint.h>

#include "od_config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as od_config_store_save(), plus the merge. The merged payload is built in the
 * workspace, so its bound is min(OD_CONFIG_MAX_SIZE, workspace_cap - header). A merge that does
 * not fit is refused OD_CONFIG_STORE_TOO_BIG with nothing written: storing the config without
 * the drive packets is the loss this exists to prevent. `carried_out` (may be NULL) is set to
 * the number of packets carried over. */
enum od_config_store_result od_config_store_save_carry(void *workspace, uint32_t workspace_cap,
                                                       const uint8_t *payload, uint32_t len,
                                                       uint8_t *carried_out);

/* True when `payload` (a whole config blob) holds a packet with id >= 0x80, or cannot be
 * walked to its end (an unknown legacy id hides the rest, so it is treated as "client knows
 * more than we do" and left alone). */
bool od_config_has_extended_packets(const uint8_t *payload, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* OD_CONFIG_CARRY_H */
