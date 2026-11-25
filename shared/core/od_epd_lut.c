/* See od_epd_lut.h. */
#include "od_epd_lut.h"

#include <string.h>

/* --- CRC ---------------------------------------------------------------------------------- */

uint16_t od_lut_crc16(uint16_t crc, const uint8_t *d, size_t n)
{
    for (size_t i = 0u; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* --- lint --------------------------------------------------------------------------------- */

/* S6-800: 8 B header + LUTC + 8 slots of 88 B. The 7.09in class hangs BUSY on 8 zero bytes in
 * a row anywhere, or on an all-zero header with the custom-LUT bit set (handover 3.2 / 4.3).
 *
 * JD-BWRY: 7 B header, byte 7, then 6 blocks of 8 rows x 11 B at 8 + 88k. Bytes are active-low
 * scrambled binary (0xFF = 0). Row r's repeat M is the byte just before it (byte 10 of row r-1,
 * byte 7 + 88k for row 0); a row runs M x (R_A x sum TP_A + R_B x sum TP_B) frames with TP_A at
 * bytes 2-4, R_A 5, TP_B 6-8, R_B 9. The first row of a block that runs no frames must have
 * M = 0: that is the end marker, and a block that runs on into rows with M != 0 and no time
 * never releases BUSY (handover 3.5). A block whose rows 0..6 all run ends at row 7 by itself,
 * as the stock colour wave does. */
#define S6_HDR      8u
#define JD_BLOCKS   6u
#define JD_BLOCK    88u
#define JD_ROW      11u
#define JD_BODY     8u

static bool family_len_ok(uint8_t family, uint16_t len)
{
    switch (family) {
    case OD_LUT_FAMILY_S6_800:  return len == 799u || len == 800u;
    case OD_LUT_FAMILY_JD_BWRY: return len == 535u;
    case OD_LUT_FAMILY_X4_536:  return len == 535u || len == 536u;
    default:                    return len <= OD_LUT_MAX_BYTES;
    }
}

/* Active-low scrambled binary: value = 1 b4 + 2 b7 + 4 b6 + 8 b1 + 16 b0 + 32 b3 + 64 b2 +
 * 128 b5 of the inverted byte. */
static uint8_t jd_val(uint8_t v)
{
    const uint8_t i = (uint8_t)~v;

    return (uint8_t)(((i >> 4) & 1u) | (((i >> 7) & 1u) << 1) | (((i >> 6) & 1u) << 2) |
                     (((i >> 1) & 1u) << 3) | ((i & 1u) << 4) | (((i >> 3) & 1u) << 5) |
                     (((i >> 2) & 1u) << 6) | (((i >> 5) & 1u) << 7));
}

void od_lut_lint_begin(od_lut_lint_t *l, uint8_t family)
{
    memset(l, 0, sizeof(*l));
    l->family = family;
}

void od_lut_lint_feed(od_lut_lint_t *l, const uint8_t *d, uint16_t n)
{
    for (uint16_t i = 0u; i < n; i++, l->off++) {
        const uint8_t v = d[i];
        const uint16_t o = l->off;

        if (l->family == OD_LUT_FAMILY_S6_800) {
            l->zero_run = v == 0u ? (uint8_t)(l->zero_run + 1u) : 0u;
            if (l->zero_run >= 8u) {
                l->bad = true;
            }
            if (o < S6_HDR && v != 0u) {
                l->hdr_nonzero = true;
            }
        } else if ((l->family == OD_LUT_FAMILY_JD_BWRY || l->family == OD_LUT_FAMILY_X4_536) &&
                   o >= JD_BODY && o < JD_BODY + JD_BLOCKS * JD_BLOCK) {
            const uint16_t rel = (uint16_t)(o - JD_BODY);
            const uint8_t r = (uint8_t)((rel % JD_BLOCK) / JD_ROW);
            const uint8_t c = (uint8_t)(rel % JD_ROW);

            if (c == 0u) {
                l->m = jd_val(l->prev);
                if (r == 0u) {
                    l->blk_done = false;
                }
            }
            if (c < sizeof(l->row)) {
                l->row[c] = v;
            }
            if (c == 9u && r < 7u && !l->blk_done) {
                const uint32_t a = (uint32_t)jd_val(l->row[5]) *
                    (uint32_t)(jd_val(l->row[2]) + jd_val(l->row[3]) + jd_val(l->row[4]));
                const uint32_t b = (uint32_t)jd_val(l->row[9]) *
                    (uint32_t)(jd_val(l->row[6]) + jd_val(l->row[7]) + jd_val(l->row[8]));

                if (l->m == 0u || a + b == 0u) {
                    if (l->m != 0u) {
                        l->bad = true;   /* a row with no time that does not end the block */
                    }
                    l->blk_done = true;
                }
            }
        }
        l->prev = v;
    }
}

bool od_lut_lint_done(const od_lut_lint_t *l)
{
    if (!family_len_ok(l->family, l->off) || l->bad) {
        return false;
    }
    return l->family != OD_LUT_FAMILY_S6_800 || l->hdr_nonzero;
}

bool od_lut_lint(uint8_t family, const uint8_t *t, uint16_t len)
{
    od_lut_lint_t l;

    if (t == NULL) {
        return false;
    }
    od_lut_lint_begin(&l, family);
    od_lut_lint_feed(&l, t, len);
    return od_lut_lint_done(&l);
}

/* OTP/MTP program and vendor-test commands (handover 4.5). Commands the stock inits of these
 * families send (0x4D on the 2.9in BWRY, 0xB6 on the 7.09in) are not here: the device cannot
 * tell a stock use from a probe, so the host gate owns those. */
bool od_lut_never_send(uint8_t family, uint8_t cmd)
{
    if (family == OD_LUT_FAMILY_NONE || family >= OD_LUT_FAMILY_COUNT) {
        return false;
    }
    return cmd == 0x90u || cmd == 0x91u || (cmd >= 0xA0u && cmd <= 0xA3u) ||
           cmd == 0x8Fu || cmd == 0xFBu || cmd == 0xFFu;
}

#if OD_EPD_LUT_CACHE > 0

/* --- raw inflate -------------------------------------------------------------------------- */

/* One-shot: the whole output is in RAM, so the output buffer is the history window and no
 * separate window is kept. Input is pulled through a small buffer from the reader. */

typedef struct {
    uint16_t count[16];
    uint16_t sym[288];
} huff_t;

typedef struct {
    od_lut_reader_t rd;
    void *ctx;
    uint16_t in_len;
    uint16_t in_off;      /* next byte to fetch from the reader */
    uint8_t  buf[32];
    uint8_t  buf_n;
    uint8_t  buf_pos;
    uint32_t bits;
    uint8_t  nbits;
    bool     bad;
    huff_t   lit;
    huff_t   dist;
    uint8_t  lens[288 + 32];
} inf_t;

static inf_t s_inf;

static int next_byte(inf_t *s)
{
    if (s->buf_pos == s->buf_n) {
        uint16_t want = (uint16_t)(s->in_len - s->in_off);

        if (want == 0u) {
            s->bad = true;
            return -1;
        }
        if (want > sizeof(s->buf)) {
            want = sizeof(s->buf);
        }
        if (s->rd(s->ctx, s->in_off, s->buf, want) != want) {
            s->bad = true;
            return -1;
        }
        s->in_off = (uint16_t)(s->in_off + want);
        s->buf_n = (uint8_t)want;
        s->buf_pos = 0u;
    }
    return s->buf[s->buf_pos++];
}

static uint32_t getbits(inf_t *s, uint8_t n)
{
    uint32_t v;

    while (s->nbits < n) {
        const int b = next_byte(s);

        if (b < 0) {
            return 0u;
        }
        s->bits |= (uint32_t)b << s->nbits;
        s->nbits = (uint8_t)(s->nbits + 8u);
    }
    v = s->bits & ((1u << n) - 1u);
    s->bits >>= n;
    s->nbits = (uint8_t)(s->nbits - n);
    return v;
}

static bool build(huff_t *h, const uint8_t *len, uint16_t n)
{
    uint16_t offs[16];
    int left = 1;

    memset(h->count, 0, sizeof(h->count));
    for (uint16_t i = 0u; i < n; i++) {
        h->count[len[i]]++;
    }
    h->count[0] = 0u;
    for (int i = 1; i < 16; i++) {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) {
            return false;   /* over-subscribed */
        }
    }
    offs[1] = 0u;
    for (int i = 1; i < 15; i++) {
        offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    }
    for (uint16_t i = 0u; i < n; i++) {
        if (len[i] != 0u) {
            h->sym[offs[len[i]]++] = i;
        }
    }
    return true;
}

static int decode(inf_t *s, const huff_t *h)
{
    int code = 0;
    int first = 0;
    int index = 0;

    for (int len = 1; len < 16; len++) {
        code |= (int)getbits(s, 1);
        if (s->bad) {
            return -1;
        }
        const int count = h->count[len];
        if (code - count < first) {
            return h->sym[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->bad = true;
    return -1;
}

static const uint16_t k_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t k_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t k_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t k_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const uint8_t k_cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static bool fixed_trees(inf_t *s)
{
    uint16_t i = 0u;

    for (; i < 144u; i++) s->lens[i] = 8u;
    for (; i < 256u; i++) s->lens[i] = 9u;
    for (; i < 280u; i++) s->lens[i] = 7u;
    for (; i < 288u; i++) s->lens[i] = 8u;
    if (!build(&s->lit, s->lens, 288u)) {
        return false;
    }
    for (i = 0u; i < 30u; i++) s->lens[i] = 5u;
    return build(&s->dist, s->lens, 30u);
}

static bool dynamic_trees(inf_t *s)
{
    const uint16_t hlit = (uint16_t)(getbits(s, 5) + 257u);
    const uint16_t hdist = (uint16_t)(getbits(s, 5) + 1u);
    const uint16_t hclen = (uint16_t)(getbits(s, 4) + 4u);
    uint16_t i;

    if (s->bad || hlit > 286u || hdist > 30u) {
        return false;
    }
    memset(s->lens, 0, 19u);
    for (i = 0u; i < hclen; i++) {
        s->lens[k_cl_order[i]] = (uint8_t)getbits(s, 3);
    }
    if (s->bad || !build(&s->lit, s->lens, 19u)) {   /* code-length code, borrowing lit */
        return false;
    }
    i = 0u;
    while (i < (uint16_t)(hlit + hdist)) {
        int sym = decode(s, &s->lit);
        uint16_t rep = 0u;
        uint8_t val = 0u;

        if (sym < 0) {
            return false;
        }
        if (sym < 16) {
            s->lens[i++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (i == 0u) {
                return false;
            }
            val = s->lens[i - 1u];
            rep = (uint16_t)(3u + getbits(s, 2));
        } else if (sym == 17) {
            rep = (uint16_t)(3u + getbits(s, 3));
        } else {
            rep = (uint16_t)(11u + getbits(s, 7));
        }
        if (s->bad || (uint32_t)i + rep > (uint32_t)hlit + hdist) {
            return false;
        }
        while (rep--) {
            s->lens[i++] = val;
        }
    }
    if (s->lens[256] == 0u) {
        return false;   /* no end-of-block code */
    }
    if (!build(&s->lit, s->lens, hlit)) {
        return false;
    }
    return build(&s->dist, &s->lens[hlit], hdist);
}

bool od_lut_inflate(od_lut_reader_t rd, void *ctx, uint16_t in_len, uint8_t *out, uint16_t cap,
                    uint16_t *out_len)
{
    inf_t *s = &s_inf;
    uint16_t o = 0u;
    uint32_t final = 0u;

    if (rd == NULL || out == NULL || in_len == 0u) {
        return false;
    }
    memset(s, 0, offsetof(inf_t, lit));
    s->rd = rd;
    s->ctx = ctx;
    s->in_len = in_len;

    do {
        uint32_t type;

        final = getbits(s, 1);
        type = getbits(s, 2);
        if (s->bad) {
            return false;
        }
        if (type == 0u) {
            uint16_t len;
            uint16_t nlen;

            s->bits = 0u;   /* byte-align */
            s->nbits = 0u;
            len = (uint16_t)getbits(s, 16);
            nlen = (uint16_t)getbits(s, 16);
            if (s->bad || (uint16_t)~nlen != len || (uint32_t)o + len > cap) {
                return false;
            }
            while (len--) {
                const int b = next_byte(s);

                if (b < 0) {
                    return false;
                }
                out[o++] = (uint8_t)b;
            }
            continue;
        }
        if (type == 1u) {
            if (!fixed_trees(s)) {
                return false;
            }
        } else if (type == 2u) {
            if (!dynamic_trees(s)) {
                return false;
            }
        } else {
            return false;
        }
        for (;;) {
            int sym = decode(s, &s->lit);

            if (sym < 0) {
                return false;
            }
            if (sym < 256) {
                if (o >= cap) {
                    return false;
                }
                out[o++] = (uint8_t)sym;
                continue;
            }
            if (sym == 256) {
                break;
            }
            sym -= 257;
            if (sym >= 29) {
                return false;
            }
            {
                const uint16_t len = (uint16_t)(k_len_base[sym] + getbits(s, k_len_extra[sym]));
                const int ds = decode(s, &s->dist);
                uint16_t dist;

                if (ds < 0 || ds >= 30) {
                    return false;
                }
                dist = (uint16_t)(k_dist_base[ds] + getbits(s, k_dist_extra[ds]));
                if (s->bad || dist > o || (uint32_t)o + len > cap) {
                    return false;
                }
                for (uint16_t k = 0u; k < len; k++, o++) {
                    out[o] = out[o - dist];
                }
            }
        }
    } while (final == 0u);

    if (out_len != NULL) {
        *out_len = o;
    }
    return true;
}

/* --- cache -------------------------------------------------------------------------------- */

static od_lut_cache_entry_t s_cache[OD_EPD_LUT_CACHE];
static uint8_t s_cache_next;

od_lut_cache_entry_t *od_lut_cache_find(uint8_t instance, uint8_t slot, bool want_inline,
                                        uint32_t gen)
{
    for (unsigned i = 0u; i < OD_EPD_LUT_CACHE; i++) {
        od_lut_cache_entry_t *c = &s_cache[i];

        if (c->used && c->instance == instance && c->slot == slot &&
            c->is_inline == want_inline && (want_inline || c->gen == gen)) {
            return c;
        }
    }
    return NULL;
}

od_lut_cache_entry_t *od_lut_cache_claim(void)
{
    for (unsigned i = 0u; i < OD_EPD_LUT_CACHE; i++) {
        if (!s_cache[i].used) {
            return &s_cache[i];
        }
    }
    for (unsigned k = 0u; k < OD_EPD_LUT_CACHE; k++) {
        od_lut_cache_entry_t *c = &s_cache[s_cache_next];

        s_cache_next = (uint8_t)((s_cache_next + 1u) % OD_EPD_LUT_CACHE);
        if (!c->is_inline) {
            c->used = false;
            return c;
        }
    }
    return NULL;
}

void od_lut_cache_drop_inline(void)
{
    for (unsigned i = 0u; i < OD_EPD_LUT_CACHE; i++) {
        if (s_cache[i].is_inline) {
            s_cache[i].used = false;
            s_cache[i].is_inline = false;
        }
    }
}

void od_lut_cache_clear(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    s_cache_next = 0u;
}

#endif /* OD_EPD_LUT_CACHE > 0 */
