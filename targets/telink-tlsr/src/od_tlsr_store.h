/* Two-bank record store on raw flash: od_hal_nvs (the config record) and od_hal_secret (the
 * three SEC2 records) are each one instance.
 *
 * A bank holds at most one record: a 16-byte header, then the payload.
 *   magic "ODS1" | seq | len | crc
 * crc is the shared CRC-32 (od_config_store.h) over seq, len and the payload. This header is
 * the medium's own framing, what makes a bank write atomic; the config record inside it is
 * od_config_store's.
 *
 * A write goes to the bank that does not hold the newest valid record, payload first and
 * header last, with seq one higher; the newest valid bank wins on read. Until the header is
 * complete the previous record is untouched and still newest, so a power cut at any point
 * leaves the old record or the new one, never a mix -- the od_hal_nvs_write contract. An erase
 * is a zero-length record written the same way (a tombstone), so it is atomic too and cannot
 * expose an older record. */
#ifndef OD_TLSR_STORE_H
#define OD_TLSR_STORE_H

#include <stdbool.h>
#include <stdint.h>

#define OD_TLSR_STORE_HDR 16u

struct od_tlsr_store {
    uint32_t bank[2];
    uint32_t bank_size;   /* bytes per bank, a whole number of sectors */
    /* cache of the newest valid record; valid == false means "scan the medium" */
    bool     valid;
    int8_t   cur;         /* -1: no valid record in either bank */
    uint32_t seq;
    uint32_t len;
};

/* 0 ok; -1 nothing stored (or a tombstone); -2 I/O. */
int od_tlsr_store_len(struct od_tlsr_store *s, uint32_t *len);
int od_tlsr_store_read(struct od_tlsr_store *s, uint32_t off, void *buf, uint32_t len);
/* len 0 writes the tombstone. */
int od_tlsr_store_write(struct od_tlsr_store *s, const void *data, uint32_t len);
/* Overwrite the superseded copy (SECURITY_V2 10.4a); the live record is kept. */
int od_tlsr_store_purge(struct od_tlsr_store *s);
/* Both stores this target has: the config record and the secrets. */
int od_tlsr_store_purge_all(void);

#endif /* OD_TLSR_STORE_H */
