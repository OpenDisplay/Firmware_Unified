/* od_sec2_rec.c -- the OS2K and OS2S byte layouts (SECURITY_V2 section 4.1).
 *
 *   OS2K  "OS2K" | ver 0x02 | origin | gen:2 LE | K_dev:16 | crc32:4 LE      28 B
 *   OS2S  "OS2S" | ver 0x01 | flags | short_boots | 0 | key_gen:2 LE | 0 0 | crc32:4 LE   16 B
 *
 * crc32 is ISO-HDLC (od_config_store_crc32) over every preceding byte, stored little-endian (the
 * spec does not name the CRC's byte order; little-endian matches the config record). A record
 * with a wrong magic, version, CRC or an out-of-range origin is NOT VALID, exactly like one that
 * is missing (section 10.1).
 */
#include "od_sec2.h"

#include "od_config_store.h"

#include <string.h>

static void put_crc(uint8_t *rec, uint32_t body_len)
{
    const uint32_t crc = od_config_store_crc32(rec, body_len);

    rec[body_len + 0u] = (uint8_t)crc;
    rec[body_len + 1u] = (uint8_t)(crc >> 8);
    rec[body_len + 2u] = (uint8_t)(crc >> 16);
    rec[body_len + 3u] = (uint8_t)(crc >> 24);
}

static bool check(const uint8_t *rec, uint32_t len, const char magic[4], uint8_t ver)
{
    const uint32_t body = len - 4u;
    const uint32_t crc = (uint32_t)rec[body] | ((uint32_t)rec[body + 1u] << 8) |
                         ((uint32_t)rec[body + 2u] << 16) | ((uint32_t)rec[body + 3u] << 24);

    return memcmp(rec, magic, 4u) == 0 && rec[4] == ver &&
           od_config_store_crc32(rec, body) == crc;
}

void od_sec2_os2k_encode(const struct od_sec2_key_rec *k, uint8_t out[OD_SEC2_OS2K_LEN])
{
    memcpy(out, "OS2K", 4u);
    out[4] = 0x02u;
    out[5] = k->origin;
    out[6] = (uint8_t)k->gen;
    out[7] = (uint8_t)(k->gen >> 8);
    memcpy(&out[8], k->key, 16u);
    put_crc(out, 24u);
}

bool od_sec2_os2k_decode(const uint8_t in[OD_SEC2_OS2K_LEN], struct od_sec2_key_rec *k)
{
    if (!check(in, OD_SEC2_OS2K_LEN, "OS2K", 0x02u) ||
        (in[5] != OD_SEC2_ORIGIN_CLAIMED && in[5] != OD_SEC2_ORIGIN_LEGACY)) {
        return false;
    }
    k->origin = in[5];
    k->gen = (uint16_t)(in[6] | ((uint16_t)in[7] << 8));
    memcpy(k->key, &in[8], 16u);
    return true;
}

void od_sec2_os2s_encode(const struct od_sec2_state_rec *s, uint8_t out[OD_SEC2_OS2S_LEN])
{
    memset(out, 0, OD_SEC2_OS2S_LEN);
    memcpy(out, "OS2S", 4u);
    out[4] = 0x01u;
    out[5] = s->flags;
    out[6] = s->short_boots;
    out[8] = (uint8_t)s->key_gen;
    out[9] = (uint8_t)(s->key_gen >> 8);
    put_crc(out, 12u);
}

bool od_sec2_os2s_decode(const uint8_t in[OD_SEC2_OS2S_LEN], struct od_sec2_state_rec *s)
{
    if (!check(in, OD_SEC2_OS2S_LEN, "OS2S", 0x01u)) {
        return false;
    }
    s->flags = in[5];
    s->short_boots = in[6];
    s->key_gen = (uint16_t)(in[8] | ((uint16_t)in[9] << 8));
    return true;
}
