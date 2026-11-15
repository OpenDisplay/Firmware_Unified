/* Two-bank record store (od_tlsr_store.h), and od_hal_nvs / od_hal_secret over it.
 * Plain C over od_tlsr_flash.h, no SDK header: tests/host/tlsr_store_test.c runs this file
 * against a simulated flash that loses power after every possible operation. */
#include "od_tlsr_store.h"

#include "od_config_store.h"
#include "od_hal_nvs.h"
#include "od_hal_secret.h"
#include "od_tlsr_flash.h"
#include "od_tlsr_layout.h"

#include <string.h>

#define STORE_MAGIC 0x3153444Fu /* "ODS1" little-endian */
#define CHUNK       64u

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* CRC over header bytes 4..11 (seq, len) and then the payload as stored in flash. */
static uint32_t crc_of_bank(uint32_t base, const uint8_t hdr[OD_TLSR_STORE_HDR], uint32_t len)
{
    uint8_t buf[CHUNK];
    uint32_t crc = od_config_store_crc32_update(0u, &hdr[4], 8u);
    uint32_t off = 0u;

    while (off < len) {
        uint32_t n = len - off < CHUNK ? len - off : CHUNK;
        od_tlsr_flash_read(base + OD_TLSR_STORE_HDR + off, buf, n);
        crc = od_config_store_crc32_update(crc, buf, n);
        off += n;
    }
    return crc;
}

/* A bank's record if it is complete and intact. */
static bool bank_valid(const struct od_tlsr_store *s, int i, uint32_t *seq, uint32_t *len)
{
    uint8_t hdr[OD_TLSR_STORE_HDR];
    uint32_t l;

    od_tlsr_flash_read(s->bank[i], hdr, sizeof hdr);
    if (rd32(hdr) != STORE_MAGIC) {
        return false;
    }
    l = rd32(&hdr[8]);
    if (l > s->bank_size - OD_TLSR_STORE_HDR || rd32(&hdr[4]) == 0xFFFFFFFFu) {
        return false;
    }
    if (crc_of_bank(s->bank[i], hdr, l) != rd32(&hdr[12])) {
        return false;
    }
    *seq = rd32(&hdr[4]);
    *len = l;
    return true;
}

static void scan(struct od_tlsr_store *s)
{
    uint32_t seq[2], len[2];
    bool ok0, ok1;

    if (s->valid) {
        return;
    }
    ok0 = bank_valid(s, 0, &seq[0], &len[0]);
    ok1 = bank_valid(s, 1, &seq[1], &len[1]);
    if (ok0 && ok1) {
        s->cur = (int8_t)(seq[1] > seq[0] ? 1 : 0);
    } else if (ok0 || ok1) {
        s->cur = (int8_t)(ok1 ? 1 : 0);
    } else {
        s->cur = -1;
    }
    if (s->cur >= 0) {
        s->seq = seq[s->cur];
        s->len = len[s->cur];
    }
    s->valid = true;
}

int od_tlsr_store_len(struct od_tlsr_store *s, uint32_t *len)
{
    scan(s);
    *len = 0u;
    if (s->cur < 0 || s->len == 0u) {
        return -1;
    }
    *len = s->len;
    return 0;
}

int od_tlsr_store_read(struct od_tlsr_store *s, uint32_t off, void *buf, uint32_t len)
{
    uint32_t have;

    if (od_tlsr_store_len(s, &have) != 0) {
        return -1;
    }
    if (off > have || len > have - off) {
        return -3;
    }
    od_tlsr_flash_read(s->bank[s->cur] + OD_TLSR_STORE_HDR + off, (uint8_t *)buf, len);
    return 0;
}

int od_tlsr_store_write(struct od_tlsr_store *s, const void *data, uint32_t len)
{
    uint8_t hdr[OD_TLSR_STORE_HDR];
    uint32_t seq, target, off, got_seq = 0u, got_len = 0u;
    int dst;

    if (len > s->bank_size - OD_TLSR_STORE_HDR || (len != 0u && data == NULL) ||
        !od_tlsr_flash_supply_ok()) {
        return -2;
    }
    scan(s);
    dst = s->cur == 0 ? 1 : 0;
    seq = s->cur >= 0 ? s->seq + 1u : 1u;
    target = s->bank[dst];
    s->valid = false; /* re-read the medium after this, whatever happens */

    for (off = 0u; off < s->bank_size; off += OD_TLSR_SECTOR) {
        if (!od_tlsr_flash_erase(target + off)) {
            return -2;
        }
    }
    if (len != 0u && !od_tlsr_flash_program(target + OD_TLSR_STORE_HDR, (const uint8_t *)data, len)) {
        return -2;
    }
    wr32(hdr, STORE_MAGIC);
    wr32(&hdr[4], seq);
    wr32(&hdr[8], len);
    wr32(&hdr[12], crc_of_bank(target, hdr, len));
    /* The payload as read back is what the CRC covers, so a program that did not take shows
     * up as a mismatch against the caller's bytes below rather than as a valid record. */
    if (len != 0u) {
        uint8_t buf[CHUNK];
        for (off = 0u; off < len; off += CHUNK) {
            uint32_t n = len - off < CHUNK ? len - off : CHUNK;
            od_tlsr_flash_read(target + OD_TLSR_STORE_HDR + off, buf, n);
            if (memcmp(buf, (const uint8_t *)data + off, n) != 0) {
                return -2;
            }
        }
    }
    if (!od_tlsr_flash_program(target, hdr, sizeof hdr)) {
        return -2;
    }
    if (!bank_valid(s, dst, &got_seq, &got_len) || got_seq != seq || got_len != len) {
        return -2;
    }
    return 0;
}

/* Destroy the superseded copy: the bank that is not newest still holds the previous record.
 * Copying the live record over it (a write with seq + 1) leaves only current bytes in both
 * banks, through the same power-safe write path. With nothing live, both banks are erased. */
int od_tlsr_store_purge(struct od_tlsr_store *s)
{
    uint8_t hdr[OD_TLSR_STORE_HDR], buf[CHUNK];
    uint32_t off, src, dst, got_seq = 0u, got_len = 0u;
    int i;

    if (!od_tlsr_flash_supply_ok()) {
        return -2;
    }
    scan(s);
    s->valid = false;
    if (s->cur < 0) {
        for (i = 0; i < 2; ++i) {
            for (off = 0u; off < s->bank_size; off += OD_TLSR_SECTOR) {
                if (!od_tlsr_flash_erase(s->bank[i] + off)) {
                    return -2;
                }
            }
        }
        return 0;
    }
    src = s->bank[s->cur];
    i = s->cur == 0 ? 1 : 0;
    dst = s->bank[i];
    for (off = 0u; off < s->bank_size; off += OD_TLSR_SECTOR) {
        if (!od_tlsr_flash_erase(dst + off)) {
            return -2;
        }
    }
    for (off = 0u; off < s->len; off += CHUNK) {
        uint32_t n = s->len - off < CHUNK ? s->len - off : CHUNK;
        od_tlsr_flash_read(src + OD_TLSR_STORE_HDR + off, buf, n);
        if (!od_tlsr_flash_program(dst + OD_TLSR_STORE_HDR + off, buf, n)) {
            return -2;
        }
    }
    wr32(hdr, STORE_MAGIC);
    wr32(&hdr[4], s->seq + 1u);
    wr32(&hdr[8], s->len);
    wr32(&hdr[12], crc_of_bank(dst, hdr, s->len));
    if (!od_tlsr_flash_program(dst, hdr, sizeof hdr) ||
        !bank_valid(s, i, &got_seq, &got_len) || got_seq != s->seq + 1u) {
        return -2;
    }
    return 0;
}

/* ---- od_hal_nvs: the config record ---- */

static struct od_tlsr_store s_cfg = {
    { OD_TLSR_CFG_BANK_A, OD_TLSR_CFG_BANK_B }, OD_TLSR_CFG_BANK_SIZE, false, -1, 0u, 0u
};

int od_hal_nvs_init(void)
{
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_size(uint32_t *len_out)
{
    if (len_out == NULL) {
        return OD_HAL_NVS_EIO;
    }
    return od_tlsr_store_len(&s_cfg, len_out) == 0 ? OD_HAL_NVS_OK : OD_HAL_NVS_ENOENT;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    int rc;

    if (buf == NULL && len != 0u) {
        return OD_HAL_NVS_EIO;
    }
    rc = od_tlsr_store_read(&s_cfg, offset, buf, len);
    return rc == 0 ? OD_HAL_NVS_OK : rc == -1 ? OD_HAL_NVS_ENOENT : OD_HAL_NVS_E2BIG;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
    if (record == NULL || len == 0u) {
        return OD_HAL_NVS_EIO;
    }
    return od_tlsr_store_write(&s_cfg, record, len) == 0 ? OD_HAL_NVS_OK : OD_HAL_NVS_EIO;
}

int od_hal_nvs_erase(void)
{
    uint32_t len;

    if (od_tlsr_store_len(&s_cfg, &len) != 0) {
        return OD_HAL_NVS_OK;
    }
    return od_tlsr_store_write(&s_cfg, NULL, 0u) == 0 ? OD_HAL_NVS_OK : OD_HAL_NVS_EIO;
}

/* ---- od_hal_secret: the three SEC2 records, one store record holding all of them ----
 * Per id: a length byte (0 = absent) and up to SECRET_CAP bytes. One write rewrites the whole
 * record, so each id's update is as atomic as the store's. After them, only on a tag that has
 * needed one, the fallback address: a length byte (6) and the address. Without it the record
 * is SECRET_REC bytes, as on every tag before the slot existed. */

#define SECRET_IDS 3u
#define SECRET_CAP 32u
#define SECRET_REC (SECRET_IDS * (1u + SECRET_CAP))
#define MAC_SLOT   7u
#define SECRET_REC_MAC (SECRET_REC + MAC_SLOT)

static struct od_tlsr_store s_sec = {
    { OD_TLSR_SEC_BANK_A, OD_TLSR_SEC_BANK_B }, OD_TLSR_SEC_BANK_SIZE, false, -1, 0u, 0u
};

static bool secret_load(uint8_t rec[SECRET_REC_MAC])
{
    uint32_t len;
    int rc = od_tlsr_store_len(&s_sec, &len);

    memset(rec, 0, SECRET_REC_MAC);
    if (rc != 0) {
        return true; /* nothing stored: every id absent */
    }
    if (len != SECRET_REC && len != SECRET_REC_MAC) {
        return false;
    }
    return od_tlsr_store_read(&s_sec, 0u, rec, len) == 0;
}

static bool secret_save(const uint8_t rec[SECRET_REC_MAC])
{
    const uint32_t len = rec[SECRET_REC] != 0u ? SECRET_REC_MAC : SECRET_REC;

    return od_tlsr_store_write(&s_sec, rec, len) == 0;
}

bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *buf, uint16_t len)
{
    uint8_t rec[SECRET_REC_MAC];
    const uint8_t *slot;

    if (buf == NULL || (unsigned)id >= SECRET_IDS || !secret_load(rec)) {
        return false;
    }
    slot = &rec[(unsigned)id * (1u + SECRET_CAP)];
    if (slot[0] == 0u || slot[0] != len) {
        return false;
    }
    memcpy(buf, &slot[1], len);
    return true;
}

static bool secret_store(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len)
{
    uint8_t rec[SECRET_REC_MAC];
    uint8_t *slot;

    if ((unsigned)id >= SECRET_IDS || len > SECRET_CAP || !secret_load(rec)) {
        return false;
    }
    slot = &rec[(unsigned)id * (1u + SECRET_CAP)];
    if (buf == NULL && slot[0] == 0u) {
        return true; /* erase of an absent id */
    }
    memset(slot, 0, 1u + SECRET_CAP);
    if (buf != NULL) {
        slot[0] = (uint8_t)len;
        memcpy(&slot[1], buf, len);
    }
    return secret_save(rec);
}

bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len)
{
    return buf != NULL && len != 0u && secret_store(id, buf, len);
}

bool od_hal_secret_erase(enum od_hal_secret_id id)
{
    return secret_store(id, NULL, 0u);
}

bool od_tlsr_store_mac_read(uint8_t mac[6])
{
    uint8_t rec[SECRET_REC_MAC];

    if (mac == NULL || !secret_load(rec) || rec[SECRET_REC] != 6u) {
        return false;
    }
    memcpy(mac, &rec[SECRET_REC + 1u], 6u);
    return true;
}

bool od_tlsr_store_mac_write(const uint8_t mac[6])
{
    uint8_t rec[SECRET_REC_MAC];

    if (mac == NULL || !secret_load(rec)) {
        return false;
    }
    if (rec[SECRET_REC] == 6u && memcmp(&rec[SECRET_REC + 1u], mac, 6u) == 0) {
        return true;
    }
    rec[SECRET_REC] = 6u;
    memcpy(&rec[SECRET_REC + 1u], mac, 6u);
    return secret_save(rec);
}

int od_tlsr_store_purge_all(void)
{
    int a = od_tlsr_store_purge(&s_cfg);
    int b = od_tlsr_store_purge(&s_sec);
    return (a == 0 && b == 0) ? 0 : -2;
}
