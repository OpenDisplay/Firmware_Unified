/* Drive-packet carry-over on config write. See od_config_carry.h. */

#include "od_config_carry.h"

#include <string.h>

#include "od_config_tlv.h"
#include "od_hal_nvs.h"
#include "od_log.h"
#include "od_span.h"

#define LP_HDR 4u   /* [num][id][len:2 LE] */

static bool is_drive(uint8_t id)
{
    return id >= OD_PKT_DISPLAY_MODE && id <= OD_PKT_DISPLAY_PANEL;
}

struct incoming {
    const uint8_t *base;
    uint32_t       end;     /* payload offset just past the last whole packet */
    bool           ext;
};

static bool note_packet(void *ctx, uint8_t packet_id, od_span_t body)
{
    struct incoming *in = (struct incoming *)ctx;

    if (packet_id >= OD_CFG_TLV_LEN_PREFIXED_MIN) {
        in->ext = true;
    }
    in->end = (uint32_t)(body.p - in->base) + (uint32_t)body.n;
    return true;
}

static bool walk_incoming(const uint8_t *payload, uint32_t len, struct incoming *in)
{
    uint8_t unknown = 0u;

    in->base = payload;
    in->end = OD_CFG_TLV_HEADER_LEN;
    in->ext = false;
    return od_config_tlv_walk(od_span_make(payload, len), note_packet, in, NULL, &unknown) ==
               OD_CFG_TLV_OK && unknown == 0u;
}

bool od_config_has_extended_packets(const uint8_t *payload, uint32_t len)
{
    struct incoming in;

    return !walk_incoming(payload, len, &in) || in.ext;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool rd(uint32_t payload_off, void *dst, uint32_t n)
{
    return od_hal_nvs_read(OD_CONFIG_STORE_HEADER_SIZE + payload_off, dst, n) == OD_HAL_NVS_OK;
}

/* Walk the stored payload [3, data_len - 2) straight off the medium, the same framing as
 * od_cfg_index but with no entry cap. *extra and *count get the drive packets' size (header
 * included) and number; with dst != NULL each is also copied there, at most `room` bytes.
 * Zero-length bodies are skipped, as the index skips them. */
static enum od_config_store_result stored_drive(uint32_t data_len, uint8_t *dst, uint32_t room,
                                                uint32_t *extra, uint8_t *count)
{
    uint32_t off = OD_CFG_TLV_HEADER_LEN;
    const uint32_t body_end = data_len - OD_CFG_TLV_CRC_LEN;

    *extra = 0u;
    *count = 0u;
    while (off + 2u <= body_end) {
        uint8_t  ph[LP_HDR];
        uint32_t hdr_sz = 2u;
        uint32_t bsz;

        if (!rd(off, ph, 2u)) {
            return OD_CONFIG_STORE_IO;
        }
        if (ph[1] >= OD_CFG_TLV_LEN_PREFIXED_MIN) {
            if (off + LP_HDR > body_end) {
                return OD_CONFIG_STORE_UNSAFE_CARRY;
            }
            if (!rd(off, ph, LP_HDR)) {
                return OD_CONFIG_STORE_IO;
            }
            hdr_sz = LP_HDR;
            bsz = (uint32_t)ph[2] | ((uint32_t)ph[3] << 8);
        } else {
            bsz = od_config_tlv_body_size(ph[1]);
            if (bsz == 0u) {
                return OD_CONFIG_STORE_UNSAFE_CARRY;   /* unknown legacy id hides the tail */
            }
        }
        if (bsz > body_end - off - hdr_sz) {
            return OD_CONFIG_STORE_UNSAFE_CARRY;
        }
        if (is_drive(ph[1]) && bsz > 0u) {
            if (dst != NULL) {
                if (LP_HDR + bsz > room - *extra) {
                    return OD_CONFIG_STORE_TOO_BIG;
                }
                if (!rd(off, dst + *extra, LP_HDR + bsz)) {
                    return OD_CONFIG_STORE_IO;
                }
            }
            *extra += LP_HDR + bsz;
            (*count)++;
        }
        off += hdr_sz + bsz;
    }
    return OD_CONFIG_STORE_OK;
}

/* The stored record's payload length once its header and CRC-32 check, 0 when nothing is
 * stored. Same checks as od_config_store_load, read in small chunks: the workspace already
 * holds the incoming config. */
static enum od_config_store_result stored_verified(uint32_t *data_len)
{
    uint8_t  buf[32];
    uint32_t record_len = 0u;
    uint32_t n;
    uint32_t crc = 0u;
    int      rc = od_hal_nvs_size(&record_len);

    *data_len = 0u;
    if (rc == OD_HAL_NVS_ENOENT) {
        return OD_CONFIG_STORE_OK;
    }
    if (rc != OD_HAL_NVS_OK) {
        return OD_CONFIG_STORE_IO;
    }
    if (record_len < OD_CONFIG_STORE_HEADER_SIZE ||
        od_hal_nvs_read(0u, buf, OD_CONFIG_STORE_HEADER_SIZE) != OD_HAL_NVS_OK) {
        return record_len < OD_CONFIG_STORE_HEADER_SIZE ? OD_CONFIG_STORE_UNSAFE_CARRY
                                                        : OD_CONFIG_STORE_IO;
    }
    n = le32(&buf[12]);
    if (le32(&buf[0]) != OD_CONFIG_STORE_MAGIC || n > OD_CONFIG_MAX_SIZE ||
        n > record_len - OD_CONFIG_STORE_HEADER_SIZE) {
        return OD_CONFIG_STORE_UNSAFE_CARRY;
    }
    const uint32_t want = le32(&buf[8]);
    for (uint32_t off = 0u; off < n;) {
        const uint32_t k = (n - off) < sizeof buf ? (n - off) : (uint32_t)sizeof buf;

        if (!rd(off, buf, k)) {
            return OD_CONFIG_STORE_IO;
        }
        crc = od_config_store_crc32_update(crc, buf, k);
        off += k;
    }
    if (crc != want) {
        return OD_CONFIG_STORE_UNSAFE_CARRY;
    }
    *data_len = n;
    return OD_CONFIG_STORE_OK;
}

enum od_config_store_result od_config_store_save_carry(void *workspace, uint32_t workspace_cap,
                                                       const uint8_t *payload, uint32_t len,
                                                       uint8_t *carried_out)
{
    struct incoming in;
    uint8_t *slot;
    uint32_t limit;
    uint32_t stored_len;
    uint32_t extra = 0u;
    uint32_t tail;
    uint32_t pos;
    uint8_t  tail_buf[OD_CFG_TLV_CRC_LEN + 1u];
    uint8_t  carried = 0u;
    bool     crc_ok;
    enum od_config_store_result rc;

    if (carried_out != NULL) {
        *carried_out = 0u;
    }
    if (workspace == NULL || payload == NULL ||
        len < OD_CFG_TLV_HEADER_LEN + OD_CFG_TLV_CRC_LEN ||
        workspace_cap < OD_CONFIG_STORE_HEADER_SIZE ||
        !walk_incoming(payload, len, &in) || in.ext) {
        return od_config_store_save(workspace, workspace_cap, payload, len);
    }

    /* FAIL CLOSED. A stored record that cannot be read, fails its CRC-32 or cannot be walked
     * to its end is refused (IO / UNSAFE_CARRY) with nothing written: storing the legacy
     * config would drop whatever drive data it held, and carrying from it would launder bad
     * bytes into a record with a valid CRC. A 3.x client (any 0x80+ packet) is not affected. */
    rc = stored_verified(&stored_len);
    if (rc == OD_CONFIG_STORE_OK && stored_len >= OD_CFG_TLV_HEADER_LEN + OD_CFG_TLV_CRC_LEN) {
        rc = stored_drive(stored_len, NULL, 0u, &extra, &carried);
    }
    if (rc != OD_CONFIG_STORE_OK) {
        od_log_error("config carry: stored config unreadable or unsound (%d), refused",
                     (int)rc);
        return rc;
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
     * drive packets are inserted in place. Only the slot is touched before the save: on BG22
     * the header words in front of it are the live assembler state.
     *
     * The packets go right after the last incoming packet, not before the CRC: the walk may
     * have stopped short of it on a stray byte, and anything placed after that byte would be
     * unreachable. What follows the last packet (that byte, then the CRC) moves behind them. */
    slot = od_config_store_payload(workspace);
    if (payload != slot) {
        memmove(slot, payload, len);
    }
    tail = len - in.end;
    if (tail > sizeof tail_buf) {
        return OD_CONFIG_STORE_UNSAFE_CARRY;    /* the TLV walk leaves at most one byte */
    }
    memcpy(tail_buf, slot + in.end, tail);
    crc_ok = od_config_tlv_crc16(od_span_make(slot, len - OD_CFG_TLV_CRC_LEN)) ==
             (uint16_t)((uint16_t)slot[len - 2u] | ((uint16_t)slot[len - 1u] << 8));

    rc = stored_drive(stored_len, slot + in.end, extra, &extra, &carried);
    if (rc != OD_CONFIG_STORE_OK) {
        od_log_error("config carry: reading stored drive packets failed, refused");
        return rc;
    }
    pos = in.end + extra;
    memcpy(slot + pos, tail_buf, tail);
    pos += tail;

    /* The CRC-16 is advisory, but a merged record should check if the incoming one did; an
     * incoming mismatch is kept as a mismatch rather than laundered. */
    if (crc_ok) {
        const uint16_t c = od_config_tlv_crc16(od_span_make(slot, pos - OD_CFG_TLV_CRC_LEN));

        slot[pos - 2u] = (uint8_t)(c & 0xFFu);
        slot[pos - 1u] = (uint8_t)(c >> 8);
    }

    od_log_info("config carry: kept %u drive packets (%u B) from the stored config",
                (unsigned)carried, (unsigned)extra);
    if (carried_out != NULL) {
        *carried_out = carried;
    }
    return od_config_store_save(workspace, workspace_cap, slot, pos);
}
