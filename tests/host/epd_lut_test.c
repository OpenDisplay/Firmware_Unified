/* epd_lut: od_epd_lut -- the raw-deflate slot inflater, the per-family table lint, the never-send
 * list and the RAM cache -- against every waveform in panels/waves (lut_vectors.inc, generated
 * by gen_lut_vectors.py with Python's zlib). */
#include "od_check.h"

#include "od_epd_lut.h"

#include <stdio.h>
#include <string.h>

#include "lut_vectors.inc"

typedef struct {
    const uint8_t *p;
    uint16_t n;
    unsigned reads;
} buf_src_t;

static uint16_t buf_reader(void *ctx, uint16_t off, uint8_t *dst, uint16_t len)
{
    buf_src_t *s = (buf_src_t *)ctx;

    s->reads++;
    if ((uint32_t)off + len > s->n) {
        return 0u;
    }
    memcpy(dst, s->p + off, len);
    return len;
}

static bool inflate_buf(const uint8_t *z, uint16_t zn, uint8_t *out, uint16_t cap,
                        uint16_t *out_n)
{
    buf_src_t src = { z, zn, 0u };

    return od_lut_inflate(buf_reader, &src, zn, out, cap, out_n);
}

static void test_inflate_vectors(void)
{
    static uint8_t out[OD_LUT_MAX_BYTES];

    CASE("every panels/waves table inflates byte-exact from dynamic, fixed and stored deflate");
    CHECK(sizeof k_lut_vectors / sizeof k_lut_vectors[0] >= 9u);
    for (size_t v = 0; v < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; v++) {
        const lut_vector_t *t = &k_lut_vectors[v];

        for (int e = 0; e < 3; e++) {
            uint16_t n = 0u;

            memset(out, 0xA5, sizeof out);
            if (!inflate_buf(t->z[e], t->z_len[e], out, sizeof out, &n) || n != t->raw_len ||
                memcmp(out, t->raw, n) != 0) {
                printf("  %s encoding %d: n=%u want %u\n", t->name, e, (unsigned)n,
                       (unsigned)t->raw_len);
                CHECK(false);
            }
        }
    }

    CASE("an output cap below the table refuses instead of truncating");
    {
        const lut_vector_t *t = &k_lut_vectors[0];
        uint16_t n = 0u;

        for (int e = 0; e < 3; e++) {
            CHECK(!inflate_buf(t->z[e], t->z_len[e], out, (uint16_t)(t->raw_len - 1u), &n));
        }
    }

    CASE("a truncated stream refuses");
    for (size_t v = 0; v < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; v++) {
        const lut_vector_t *t = &k_lut_vectors[v];
        uint16_t n = 0u;

        CHECK(!inflate_buf(t->z[0], (uint16_t)(t->z_len[0] / 2u), out, sizeof out, &n));
        CHECK(!inflate_buf(t->z[2], (uint16_t)(t->z_len[2] - 1u), out, sizeof out, &n));
    }

    CASE("garbage refuses or stays inside the cap (never writes past it)");
    {
        static uint8_t junk[300];
        static uint8_t guard[OD_LUT_MAX_BYTES + 16u];
        uint32_t x = 0x12345678u;

        for (int round = 0; round < 2000; round++) {
            uint16_t n = 0u;

            for (size_t i = 0; i < sizeof junk; i++) {
                x = x * 1103515245u + 12345u;
                junk[i] = (uint8_t)(x >> 16);
            }
            memset(guard, 0x5A, sizeof guard);
            if (inflate_buf(junk, sizeof junk, guard, OD_LUT_MAX_BYTES, &n)) {
                CHECK(n <= OD_LUT_MAX_BYTES);
            }
            for (size_t i = OD_LUT_MAX_BYTES; i < sizeof guard; i++) {
                if (guard[i] != 0x5Au) {
                    CHECK(false);
                    break;
                }
            }
        }
    }

    CASE("a stored block with a bad NLEN refuses");
    {
        const uint8_t bad[] = { 0x01, 0x03, 0x00, 0xFC, 0xFE, 'a', 'b', 'c' };
        uint16_t n = 0u;

        CHECK(!inflate_buf(bad, sizeof bad, out, sizeof out, &n));
    }
    CASE("a back-reference before the start of the output refuses");
    {
        /* fixed block: literal 'a', then length 3 distance 2 (only 1 byte of history), end */
        const uint8_t bad[] = { 0x4B, 0x04, 0x42, 0x00 };   /* zlib: "too far back" */
        uint16_t n = 0u;

        CHECK(!inflate_buf(bad, sizeof bad, out, sizeof out, &n));
    }
}

static void test_lint(void)
{
    static uint8_t t[OD_LUT_MAX_BYTES];

    CASE("every panels/waves table passes its family lint");
    for (size_t v = 0; v < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; v++) {
        const lut_vector_t *k = &k_lut_vectors[v];

        if (!od_lut_lint(k->family, k->raw, k->raw_len)) {
            printf("  %s fails family %u\n", k->name, (unsigned)k->family);
            CHECK(false);
        }
        /* The streamed lint agrees with the whole-buffer one, at any chunking. */
        {
            od_lut_lint_t l;
            uint16_t off = 0u;

            od_lut_lint_begin(&l, k->family);
            while (off < k->raw_len) {
                const uint16_t left = (uint16_t)(k->raw_len - off);
                const uint16_t c = left < 7u ? left : (uint16_t)7u;
                od_lut_lint_feed(&l, k->raw + off, c);
                off = (uint16_t)(off + c);
            }
            CHECK(od_lut_lint_done(&l));
        }
    }

    CASE("S6: 8 zero bytes in a row anywhere, or an all-zero header, fail");
    for (size_t v = 0; v < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; v++) {
        const lut_vector_t *k = &k_lut_vectors[v];

        if (k->family != OD_LUT_FAMILY_S6_800) {
            continue;
        }
        memcpy(t, k->raw, k->raw_len);
        memset(&t[400], 0, 8u);
        CHECK(!od_lut_lint(k->family, t, k->raw_len));
        memcpy(t, k->raw, k->raw_len);
        memset(&t[400], 0, 7u);
        if (t[399] != 0u && t[407] != 0u) {
            CHECK(od_lut_lint(k->family, t, k->raw_len));
        }
        memcpy(t, k->raw, k->raw_len);
        memset(t, 0, 8u);
        CHECK(!od_lut_lint(k->family, t, k->raw_len));
        CHECK(!od_lut_lint(k->family, k->raw, 535u));   /* wrong size for the family */
    }

    CASE("JD-BWRY: a block that runs into a timeless row with M != 0 fails");
    for (size_t v = 0; v < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; v++) {
        const lut_vector_t *k = &k_lut_vectors[v];

        if (k->family != OD_LUT_FAMILY_JD_BWRY) {
            continue;
        }
        /* Make every M byte of every block "1" (0xEF): no block can end before row 7. Any block
         * with fewer than 7 timed rows then has a timeless row that does not end it. */
        memcpy(t, k->raw, k->raw_len);
        for (unsigned j = 0; j < 48u; j++) {
            t[7u + 11u * j] = 0xEFu;
        }
        CHECK(!od_lut_lint(k->family, t, k->raw_len));
        CHECK(!od_lut_lint(k->family, k->raw, (uint16_t)(k->raw_len - 1u)));
    }
    CASE("x4 accepts 535 and 536 B; undeclared family checks only the ceiling");
    {
        const lut_vector_t *k = NULL;

        for (size_t v = 0; v < sizeof k_lut_vectors / sizeof k_lut_vectors[0]; v++) {
            if (k_lut_vectors[v].family == OD_LUT_FAMILY_JD_BWRY) {
                k = &k_lut_vectors[v];
            }
        }
        CHECK(k != NULL);
        if (k != NULL) {
            memcpy(t, k->raw, k->raw_len);
            t[535] = 0xEFu;
            CHECK(od_lut_lint(OD_LUT_FAMILY_X4_536, t, 535u));
            CHECK(od_lut_lint(OD_LUT_FAMILY_X4_536, t, 536u));
        }
        memset(t, 0, sizeof t);
        CHECK(od_lut_lint(OD_LUT_FAMILY_NONE, t, OD_LUT_MAX_BYTES));
        CHECK(!od_lut_lint(OD_LUT_FAMILY_NONE, t, OD_LUT_MAX_BYTES + 1u));
    }

    CASE("never-send: OTP/MTP program commands refused per declared family only");
    {
        static const uint8_t bad[] = { 0x90, 0x91, 0xA0, 0xA1, 0xA2, 0xA3, 0x8F, 0xFB, 0xFF };
        static const uint8_t ok[] = { 0x00, 0x01, 0x04, 0x10, 0x12, 0x20, 0x30, 0x4D, 0x50,
                                      0x61, 0x82, 0x83, 0xAA, 0xB6, 0xE3 };
        for (uint8_t f = 1u; f < OD_LUT_FAMILY_COUNT; f++) {
            for (size_t i = 0; i < sizeof bad; i++) {
                CHECK(od_lut_never_send(f, bad[i]));
                CHECK(!od_lut_never_send(OD_LUT_FAMILY_NONE, bad[i]));
            }
            for (size_t i = 0; i < sizeof ok; i++) {
                CHECK(!od_lut_never_send(f, ok[i]));
            }
        }
        CHECK(!od_lut_never_send(OD_LUT_FAMILY_COUNT, 0x90));
    }
}

static void test_cache(void)
{
    CASE("cache: inline entries are never evicted; stored entries are keyed by generation");
    od_lut_cache_clear();
    {
        od_lut_cache_entry_t *a = od_lut_cache_claim();
        od_lut_cache_entry_t *b;

        CHECK(a != NULL);
        a->used = true;
        a->is_inline = true;
        a->instance = 0u;
        a->slot = 3u;
        b = od_lut_cache_claim();
        CHECK(b != NULL && b != a);
        b->used = true;
        b->is_inline = false;
        b->instance = 0u;
        b->slot = 3u;
        b->gen = 7u;
        CHECK(od_lut_cache_find(0u, 3u, true, 0u) == a);
        CHECK(od_lut_cache_find(0u, 3u, false, 7u) == b);
        CHECK(od_lut_cache_find(0u, 3u, false, 8u) == NULL);
        CHECK(od_lut_cache_find(1u, 3u, true, 0u) == NULL);
        /* Both used: the next claim must take the stored one, never the inline one. */
        CHECK(od_lut_cache_claim() == b);
        b->used = true;
        b->is_inline = true;
        CHECK(od_lut_cache_claim() == NULL);
        od_lut_cache_drop_inline();
        CHECK(od_lut_cache_find(0u, 3u, true, 0u) == NULL);
        CHECK(od_lut_cache_claim() != NULL);
    }
    od_lut_cache_clear();

    CASE("crc16 is CCITT-FALSE and chains");
    {
        const uint8_t d[] = "123456789";
        CHECK(od_lut_crc16(0xFFFFu, d, 9u) == 0x29B1u);
        CHECK(od_lut_crc16(od_lut_crc16(0xFFFFu, d, 4u), d + 4, 5u) == 0x29B1u);
    }
}

int main(void)
{
    test_inflate_vectors();
    test_lint();
    test_cache();
    return OD_CHECK_REPORT_NONEMPTY("epd_lut", 60);
}
