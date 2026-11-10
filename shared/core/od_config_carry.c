/* Drive-packet carry-over on config write. See od_config_carry.h. */

#include "od_config_carry.h"

#include <string.h>

#include "od_cfg_index.h"
#include "od_config_tlv.h"
#include "od_hal_nvs.h"
#include "od_log.h"
#include "od_span.h"

#define LP_HDR 4u   /* [num][id][len:2 LE] */

static bool is_drive(uint8_t id)
{
    return id >= OD_PKT_DISPLAY_MODE && id <= OD_PKT_DISPLAY_PANEL;
}

static bool note_extended(void *ctx, uint8_t packet_id, od_span_t body)
{
    (void)body;
    if (packet_id >= OD_CFG_TLV_LEN_PREFIXED_MIN) {
        *(bool *)ctx = true;
    }
    return true;
}

bool od_config_has_extended_packets(const uint8_t *payload, uint32_t len)
{
    bool ext = false;
    uint8_t unknown = 0u;
    const enum od_config_tlv_result rc =
        od_config_tlv_walk(od_span_make(payload, len), note_extended, &ext, NULL, &unknown);

    return ext || rc != OD_CFG_TLV_OK || unknown != 0u;
}

enum od_config_store_result od_config_store_save_carry(void *workspace, uint32_t workspace_cap,
                                                       const uint8_t *payload, uint32_t len,
                                                       uint8_t *carried_out)
{
    uint8_t *slot;
    uint32_t limit;
    uint32_t extra = 0u;
    uint32_t pos;
    uint8_t  crc_tail[OD_CFG_TLV_CRC_LEN];
    uint8_t  carried = 0u;
    bool     crc_ok;
    int      n;

    if (carried_out != NULL) {
        *carried_out = 0u;
    }
    if (workspace == NULL || payload == NULL ||
        len < OD_CFG_TLV_HEADER_LEN + OD_CFG_TLV_CRC_LEN ||
        workspace_cap < OD_CONFIG_STORE_HEADER_SIZE ||
        od_config_has_extended_packets(payload, len)) {
        return od_config_store_save(workspace, workspace_cap, payload, len);
    }

    n = od_cfg_index_build();
    for (int i = 0; i < n; ++i) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);

        if (e != NULL && is_drive(e->type)) {
            extra += LP_HDR + e->len;
        }
    }
    if (extra == 0u) {
        return od_config_store_save(workspace, workspace_cap, payload, len);
    }

    limit = workspace_cap - OD_CONFIG_STORE_HEADER_SIZE;
    if (limit > OD_CONFIG_MAX_SIZE) {
        limit = OD_CONFIG_MAX_SIZE;
    }
    if (len > limit || extra > limit - len) {
        od_log_warn("config carry: %u B config + %u B drive packets exceed %u B, refused",
                    (unsigned)len, (unsigned)extra, (unsigned)limit);
        return OD_CONFIG_STORE_TOO_BIG;
    }

    /* Into the workspace slot first (the same move od_config_store_save would make), so the
     * drive packets are appended in place. Only the slot is touched before the save: on BG22
     * the header words in front of it are the live assembler state. */
    slot = od_config_store_payload(workspace);
    if (payload != slot) {
        memmove(slot, payload, len);
    }
    crc_ok = od_config_tlv_crc16(od_span_make(slot, len - OD_CFG_TLV_CRC_LEN)) ==
             (uint16_t)((uint16_t)slot[len - 2u] | ((uint16_t)slot[len - 1u] << 8));
    memcpy(crc_tail, slot + len - OD_CFG_TLV_CRC_LEN, OD_CFG_TLV_CRC_LEN);

    pos = len - OD_CFG_TLV_CRC_LEN;
    for (int i = 0; i < n; ++i) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);

        if (e == NULL || !is_drive(e->type)) {
            continue;
        }
        /* Header and body together, straight from the medium: the body offset is in payload
         * coordinates and the length-prefixed header sits in the 4 bytes before it. */
        if (od_hal_nvs_read(OD_CONFIG_STORE_HEADER_SIZE + (uint32_t)e->offset - LP_HDR,
                            slot + pos, LP_HDR + e->len) != OD_HAL_NVS_OK) {
            od_log_error("config carry: reading stored 0x%02X failed", (unsigned)e->type);
            return OD_CONFIG_STORE_IO;
        }
        pos += LP_HDR + e->len;
        carried++;
    }

    /* The CRC-16 is advisory, but a merged record should check if the incoming one did; an
     * incoming mismatch is kept as a mismatch rather than laundered. */
    if (crc_ok) {
        const uint16_t c = od_config_tlv_crc16(od_span_make(slot, pos));

        crc_tail[0] = (uint8_t)(c & 0xFFu);
        crc_tail[1] = (uint8_t)(c >> 8);
    }
    memcpy(slot + pos, crc_tail, OD_CFG_TLV_CRC_LEN);
    pos += OD_CFG_TLV_CRC_LEN;

    od_log_info("config carry: kept %u drive packets (%u B) from the stored config",
                (unsigned)carried, (unsigned)extra);
    if (carried_out != NULL) {
        *carried_out = carried;
    }
    return od_config_store_save(workspace, workspace_cap, slot, pos);
}
