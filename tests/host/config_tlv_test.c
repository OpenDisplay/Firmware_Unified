/* config_tlv_test.c -- host tests for the shared config-blob walk.
 *
 * The walk is the pre-auth attack surface: this blob is parsed at boot from storage a client
 * can write, and on a device with encryption disabled the write path has no authentication
 * gate at all. So the cases that matter are the hostile ones -- truncation, a packet claiming
 * more than the blob holds, a length that would overflow the bounds arithmetic, and a header
 * straddling the trailing CRC.
 *
 * The CRC tests are DIFFERENTIAL against a local copy of the shipped implementation, not
 * against hand-computed constants. A checksum test written from the implementation it is
 * testing proves only that the code does what it does; reproducing the shipped algorithm
 * independently is what catches a promotion that quietly changed the function.
 */
#include "od_config_tlv.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);  \
        }                                                                       \
    } while (0)

#define CASE(name) (g_case = (name))

/* ------------------------------------------------------------------------ capture callback --- */

#define MAX_SEEN 16
struct seen {
    unsigned count;
    uint8_t  ids[MAX_SEEN];
    uint16_t lens[MAX_SEEN];
    const uint8_t *bodies[MAX_SEEN];
    int      refuse_at;     /* -1 = accept everything */
};

static bool on_packet(void *ctx, uint8_t id, od_span_t body)
{
    struct seen *s = (struct seen *)ctx;
    if (s->refuse_at >= 0 && (int)s->count == s->refuse_at) {
        return false;
    }
    if (s->count < MAX_SEEN) {
        s->ids[s->count]    = id;
        s->lens[s->count]   = (uint16_t)body.n;
        s->bodies[s->count] = body.p;
    }
    s->count++;
    return true;
}

static void seen_init(struct seen *s) { memset(s, 0, sizeof *s); s->refuse_at = -1; }

/* ---------------------------------------------------------------------------- blob builder --- */

static uint8_t g_blob[4096];
static uint32_t g_len;

static void blob_begin(uint8_t version)
{
    memset(g_blob, 0, sizeof g_blob);
    g_blob[0] = 0; g_blob[1] = 0;      /* container length field, zeroed by the CRC anyway */
    g_blob[2] = version;
    g_len = 3;
}

static void blob_add(uint8_t id, uint8_t fill)
{
    const uint16_t body = od_config_tlv_body_size(id);
    g_blob[g_len++] = 0;               /* reserved */
    g_blob[g_len++] = id;
    memset(&g_blob[g_len], fill, body);
    g_len += body;
}

static void blob_end(void)
{
    g_blob[g_len++] = 0x00;            /* CRC placeholder -- content irrelevant to the walk */
    g_blob[g_len++] = 0x00;
}

/* A length-prefixed packet: [reserved][id][length:2 LE][body]. `declared` is written to the
 * wire verbatim so a test can declare a length the blob does not actually carry. */
static void blob_add_ex(uint8_t id, uint16_t declared, uint16_t actual, uint8_t fill)
{
    g_blob[g_len++] = 0;               /* reserved */
    g_blob[g_len++] = id;
    g_blob[g_len++] = (uint8_t)(declared & 0xFFu);
    g_blob[g_len++] = (uint8_t)(declared >> 8);
    memset(&g_blob[g_len], fill, actual);
    g_len += actual;
}

/* --------------------------------------------------------------------------------- the walk --- */

static void test_too_short(void)
{
    struct seen s; seen_init(&s);
    CASE("blobs with no room for header + CRC are refused");
    for (uint32_t n = 0; n < OD_CFG_TLV_HEADER_LEN + OD_CFG_TLV_CRC_LEN; ++n) {
        CHECK(od_config_tlv_walk(od_span_make(g_blob, n), on_packet, &s, NULL, NULL) == OD_CFG_TLV_TOO_SHORT);
    }
    CHECK(s.count == 0);

    CASE("null arguments are refused, not dereferenced");
    CHECK(od_config_tlv_walk(od_span_make(NULL, 100), on_packet, &s, NULL, NULL) == OD_CFG_TLV_TOO_SHORT);
    CHECK(od_config_tlv_walk(od_span_make(g_blob, 100), NULL, &s, NULL, NULL) == OD_CFG_TLV_TOO_SHORT);
}

static void test_walks_known_packets(void)
{
    struct seen s; seen_init(&s);
    uint8_t version = 0;

    CASE("a well-formed blob yields every packet, in order, with exact bodies");
    blob_begin(0x07);
    blob_add(0x01, 0xA1);
    blob_add(0x04, 0xA2);
    blob_add(0x27, 0xA3);
    blob_end();

    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, &version, NULL) == OD_CFG_TLV_OK);
    CHECK(version == 0x07);
    CHECK(s.count == 3);
    CHECK(s.ids[0] == 0x01 && s.lens[0] == od_config_tlv_body_size(0x01));
    CHECK(s.ids[1] == 0x04 && s.lens[1] == od_config_tlv_body_size(0x04));
    CHECK(s.ids[2] == 0x27 && s.lens[2] == od_config_tlv_body_size(0x27));
    CHECK(s.bodies[0][0] == 0xA1);
    CHECK(s.bodies[1][0] == 0xA2);
    CHECK(s.bodies[2][0] == 0xA3);

    CASE("every body lies inside the blob, clear of the trailing CRC");
    for (unsigned i = 0; i < s.count; ++i) {
        const uint32_t start = (uint32_t)(s.bodies[i] - g_blob);
        CHECK(start + s.lens[i] <= g_len - OD_CFG_TLV_CRC_LEN);
    }
}

static void test_repeatable_packets(void)
{
    struct seen s; seen_init(&s);
    CASE("repeated packet ids are all delivered -- the cap is the target's business");
    blob_begin(1);
    for (unsigned i = 0; i < 6; ++i) {
        blob_add(0x20, (uint8_t)(0xB0 + i));
    }
    blob_end();
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL) == OD_CFG_TLV_OK);
    CHECK(s.count == 6);
    for (unsigned i = 0; i < 6; ++i) {
        CHECK(s.bodies[i][0] == (uint8_t)(0xB0 + i));
    }
}

static void test_unknown_id_ends_the_walk(void)
{
    struct seen s; seen_init(&s);
    /* Current fleet behaviour, preserved deliberately: an unknown id abandons the remainder
     * rather than skipping past it. The size-table skip model is deferred (D4), and this test
     * is what will fail loudly if a partial version of it arrives by accident. */
    CASE("an unknown id ends the walk cleanly, and later packets are NOT delivered");
    blob_begin(1);
    blob_add(0x01, 0xC1);
    g_blob[g_len++] = 0; g_blob[g_len++] = 0x7E;   /* unknown id */
    blob_add(0x04, 0xC2);
    blob_end();

    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL) == OD_CFG_TLV_OK);
    CHECK(s.count == 1);
    CHECK(s.ids[0] == 0x01);
}

static void test_truncated_packet(void)
{
    struct seen s;
    CASE("a packet claiming more than the blob holds is TRUNCATED, not read");
    /* Build a valid blob, then shrink it so the last packet no longer fits. Every byte the
     * walk could read past the end is one an attacker chose. */
    blob_begin(1);
    blob_add(0x01, 0xD1);
    blob_add(0x2C, 0xD2);          /* 288-byte body: plenty to cut into */
    blob_end();

    for (uint32_t cut = 1; cut < 200u; cut += 17u) {
        seen_init(&s);
        const uint32_t shorter = g_len - cut;
        const enum od_config_tlv_result r =
            od_config_tlv_walk(od_span_make(g_blob, shorter), on_packet, &s, NULL, NULL);
        CHECK(r == OD_CFG_TLV_TRUNCATED);
        CHECK(s.count == 1);       /* the first packet still fit; the second did not */
    }
}

static void test_header_straddling_the_crc(void)
{
    struct seen s; seen_init(&s);
    /* A trailing byte pair that is really the CRC must never be read as a packet header. This
     * is the confusion a single body_end bound exists to prevent. */
    /* THE CRC BYTES ARE CHOSEN SO THIS CANNOT PASS BY ACCIDENT. An earlier version used a zero
     * stray byte and zero CRC placeholders, so deleting the two-byte header check still gave
     * reserved=0x00, id=0x00 -- an UNKNOWN id, which ends the walk with the same count the
     * assertions expected. The test passed with the protection removed. Here the second CRC
     * FIRST CRC byte is 0x01, a KNOWN id -- the id is read from there, not the second byte,
     * because the stray byte ahead of it is consumed as `reserved`. With the header check
     * dropped the walk reads a packet header straight out of the CRC field and reports a
     * second packet, so the count assertion below fails. Verified by injecting exactly that
     * mutation; the first attempt at this test used zero CRC bytes and still passed, because
     * id 0x00 is unknown and ended the walk with the expected count. */
    CASE("a lone byte before the CRC is not parsed as a packet header");
    blob_begin(1);
    blob_add(0x01, 0xE1);
    g_blob[g_len++] = 0x00;        /* one stray byte -- would be read as `reserved` */
    g_blob[g_len++] = 0x01;        /* CRC byte 0 -- would be read as a KNOWN packet id */
    g_blob[g_len++] = 0x00;        /* CRC byte 1 */

    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL) == OD_CFG_TLV_OK);
    CHECK(s.count == 1);
}

static void test_callback_refusal(void)
{
    struct seen s; seen_init(&s);
    CASE("a refusing callback aborts the walk");
    blob_begin(1);
    blob_add(0x01, 0xF1);
    blob_add(0x04, 0xF2);
    blob_add(0x27, 0xF3);
    blob_end();
    s.refuse_at = 1;
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL) == OD_CFG_TLV_REJECTED);
    CHECK(s.count == 1);           /* the refused one is not recorded */
}

static void test_unknown_id_is_reported(void)
{
    struct seen s; seen_init(&s);
    uint8_t stopper = 0xFF;

    CASE("the id that ended the walk is reported, so the caller can still log it");
    blob_begin(1);
    blob_add(0x01, 0xC1);
    g_blob[g_len++] = 0; g_blob[g_len++] = 0x7E;
    blob_end();
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, &stopper) == OD_CFG_TLV_OK);
    CHECK(stopper == 0x7E);

    CASE("a walk that runs to the end reports no stopper");
    seen_init(&s);
    stopper = 0xFF;
    blob_begin(1);
    blob_add(0x01, 0xC2);
    blob_end();
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, &stopper) == OD_CFG_TLV_OK);
    CHECK(stopper == 0x00);
}

static void test_body_size_table(void)
{
    CASE("body sizes come from the canonical structs");
    CHECK(od_config_tlv_body_size(0x01) == sizeof(struct SystemConfig));
    CHECK(od_config_tlv_body_size(0x26) == sizeof(struct WifiConfig));
    CHECK(od_config_tlv_body_size(0x27) == sizeof(struct SecurityConfig));
    CHECK(od_config_tlv_body_size(0x2A) == sizeof(struct NfcConfig));
    CHECK(od_config_tlv_body_size(0x2C) == sizeof(struct DataExtended));
    CASE("unknown ids report zero");
    CHECK(od_config_tlv_body_size(0x00) == 0);
    CHECK(od_config_tlv_body_size(0x7E) == 0);
    CHECK(od_config_tlv_body_size(0xFF) == 0);
    /* 0x22 is genuinely unassigned -- no struct, no id in the canonical contract. That is the
     * only reason a size lookup may report 0.
     *
     * This assertion used to include 0x2A and claimed it reported 0 because "no target here
     * implements NFC". Two of the three did, and the rule settled 2026-08-13 is that a target
     * parses and stores every canonical packet whether or not it can act on it -- so 0x2A is in
     * the table and is asserted with the known ids above. Kept as a note because the mistake is
     * instructive: a size table transcribed from ONE target's parser encodes that target's
     * hardware as if it were the protocol. */
    CHECK(od_config_tlv_body_size(0x22) == 0);
}

/* -------------------------------------------------------------------------------- the CRC --- */

/* An INDEPENDENT transcription of the shipped algorithm (config_parser.cpp
 * config_toolbox_outer_crc16 / Firmware_NRF / Firmware_Silabs), written from its description
 * rather than by calling the function under test. */
static uint16_t ref_feed(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t ref_crc(const uint8_t *d, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    if (n < 2u) {
        for (uint32_t i = 0; i < n; i++) crc = ref_feed(crc, d[i]);
        return crc;
    }
    crc = ref_feed(crc, 0);
    crc = ref_feed(crc, 0);
    for (uint32_t i = 2; i < n; i++) crc = ref_feed(crc, d[i]);
    return crc;
}

static void test_crc_matches_shipped(void)
{
    CASE("CRC matches the shipped toolbox-outer algorithm");
    uint8_t buf[512];
    for (unsigned n = 0; n <= 300u; n += 7u) {
        for (unsigned i = 0; i < n; ++i) {
            buf[i] = (uint8_t)((i * 31u + n) & 0xFFu);
        }
        CHECK(od_config_tlv_crc16(od_span_make(buf, n)) == ref_crc(buf, n));
    }

    /* The length-independence property is the whole reason the first two bytes are zeroed.
     * If a promotion ever drops that, this is what says so. */
    CASE("the leading length field does not affect the CRC");
    for (unsigned i = 0; i < 64u; ++i) buf[i] = (uint8_t)i;
    const uint16_t a = od_config_tlv_crc16(od_span_make(buf, 64u));
    buf[0] = 0xAB; buf[1] = 0xCD;
    CHECK(od_config_tlv_crc16(od_span_make(buf, 64u)) == a);
    buf[2] = 0xFF;
    CHECK(od_config_tlv_crc16(od_span_make(buf, 64u)) != a);   /* ...but real body bytes do */

    CASE("null and empty inputs are defined");
    CHECK(od_config_tlv_crc16(od_span_make(NULL, 10)) == 0xFFFFu);
    CHECK(od_config_tlv_crc16(od_span_make(buf, 0)) == 0xFFFFu);
}

/* ------------------------------------------------------------------ length-prefixed range --- */

/* The property the whole 0x80+ range exists for: a config carrying a packet this build does
 * not know must still load, and every packet AFTER it must still be seen. In the frozen legacy
 * range the same situation ends the walk, because there is no length to skip by. */
static void test_unknown_length_prefixed_is_skipped(void)
{
    struct seen s; seen_init(&s);
    uint8_t ended = 0xFFu;

    CASE("an unknown 0x80+ id is stepped over and the walk continues");
    blob_begin(1);
    blob_add(0x01, 0xA1);                      /* SystemConfig */
    blob_add_ex(0xC7u, 40u, 40u, 0xD7);        /* not a real id -- must be skipped by length */
    blob_add(0x20, 0xA2);                      /* DisplayConfig, AFTER the unknown one */
    blob_end();

    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, &ended)
          == OD_CFG_TLV_OK);
    /* fn is called for every bounded packet, unknown 0x80+ included. What matters is that the
     * packet after it was reached -- in the legacy range it never would be. */
    CHECK(s.count == 3);
    CHECK(s.ids[0] == 0x01);
    CHECK(s.ids[1] == 0xC7u);
    CHECK(s.lens[1] == 40u);
    CHECK(s.ids[2] == 0x20);
    CHECK(s.lens[2] == od_config_tlv_body_size(0x20));
    /* It did not END the walk, so it must not be reported as the id that did. */
    CHECK(ended == 0u);
}

static void test_drive_packets_use_the_wire_length(void)
{
    struct seen s; seen_init(&s);

    CASE("drive packet bodies come from the wire, not from the size table");
    blob_begin(1);
    blob_add_ex(OD_PKT_DISPLAY_PANEL, 25u, 25u, 0xB1);
    blob_add_ex(OD_PKT_DISPLAY_LUT, 541u, 541u, 0xAF);   /* a full waveform slot */
    blob_add_ex(OD_PKT_DISPLAY_MODE, 63u, 63u, 0xAE);
    blob_end();

    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL)
          == OD_CFG_TLV_OK);
    CHECK(s.count == 3);
    CHECK(s.ids[0] == OD_PKT_DISPLAY_PANEL && s.lens[0] == 25u);
    CHECK(s.ids[1] == OD_PKT_DISPLAY_LUT   && s.lens[1] == 541u);
    CHECK(s.ids[2] == OD_PKT_DISPLAY_MODE  && s.lens[2] == 63u);

    /* The table must stay silent about these: their size is not its business, and a caller
     * that reads 0 as "unknown" without checking the range would reject every drive packet. */
    CHECK(od_config_tlv_body_size(OD_PKT_DISPLAY_MODE) == 0u);
    CHECK(od_config_tlv_body_size(OD_PKT_DISPLAY_LUT) == 0u);
}

/* The hostile case for this range. The declared length is attacker-controlled: the blob is
 * parsed at boot from storage a client can write. An oversized declaration must truncate the
 * walk, never be added to an offset and read past the body region. */
static void test_length_prefixed_overclaim_is_truncation(void)
{
    struct seen s; seen_init(&s);

    CASE("a 0x80+ packet claiming more than the blob holds is TRUNCATED");
    blob_begin(1);
    blob_add_ex(OD_PKT_DISPLAY_LUT, 4000u, 8u, 0xAF);   /* declares 4000, carries 8 */
    blob_end();
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL)
          == OD_CFG_TLV_TRUNCATED);
    CHECK(s.count == 0);

    CASE("0xFFFF cannot wrap the bounds arithmetic");
    seen_init(&s);
    blob_begin(1);
    blob_add_ex(OD_PKT_DISPLAY_LUT, 0xFFFFu, 8u, 0xAF);
    blob_end();
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL)
          == OD_CFG_TLV_TRUNCATED);
    CHECK(s.count == 0);

    CASE("a length field straddling the CRC tail is TRUNCATED, not read");
    seen_init(&s);
    blob_begin(1);
    g_blob[g_len++] = 0;                       /* reserved */
    g_blob[g_len++] = OD_PKT_DISPLAY_LUT;  /* id, then only ONE length byte before the CRC */
    g_blob[g_len++] = 0x10;
    blob_end();
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL)
          == OD_CFG_TLV_TRUNCATED);
    CHECK(s.count == 0);
}

static void test_zero_length_prefixed_body(void)
{
    struct seen s; seen_init(&s);

    CASE("a zero-length 0x80+ body is bounded, not an error");
    blob_begin(1);
    blob_add_ex(OD_PKT_DISPLAY_CAL, 0u, 0u, 0x00);
    blob_add(0x01, 0xA1);
    blob_end();
    CHECK(od_config_tlv_walk(od_span_make(g_blob, g_len), on_packet, &s, NULL, NULL)
          == OD_CFG_TLV_OK);
    CHECK(s.count == 2);
    CHECK(s.ids[0] == OD_PKT_DISPLAY_CAL && s.lens[0] == 0u);
    CHECK(s.ids[1] == 0x01);     /* the packet after it is still reached */
}

int main(void)
{
    test_too_short();
    test_walks_known_packets();
    test_repeatable_packets();
    test_unknown_id_ends_the_walk();
    test_truncated_packet();
    test_header_straddling_the_crc();
    test_callback_refusal();
    test_unknown_id_is_reported();
    test_body_size_table();
    test_unknown_length_prefixed_is_skipped();
    test_drive_packets_use_the_wire_length();
    test_length_prefixed_overclaim_is_truncation();
    test_zero_length_prefixed_body();
    test_crc_matches_shipped();

    printf("config_tlv: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
