/* cfg_index: od_cfg_index over a stored config record -- the map every drive, action and
 * carry-over read goes through.
 *
 * Built twice, once per config cap (4096 B on ESP32/Nordic, 3120 B on BG22), because the cap is
 * the index's only size bound. Every walk result is also checked against od_config_tlv_walk,
 * the parser od_config uses at boot: two walkers over the same bytes that disagree about where a
 * packet is are a divergence a host cannot see.
 */
#include "od_check.h"
#include "drive_fixture.h"

#include "od_cfg_index.h"

/* ------------------------------------------------------------------ differential oracle -- */

typedef struct {
    uint8_t  type, instance, sub;
    uint16_t offset, len;
} walk_pkt_t;

typedef struct {
    walk_pkt_t     pkt[64];
    unsigned       n;
    const uint8_t *base;
} walk_t;

static bool indexed_type(uint8_t id)
{
    if (id >= OD_CFG_TLV_LEN_PREFIXED_MIN) {
        return id == OD_PKT_DISPLAY_MODE || id == OD_PKT_DISPLAY_LUT ||
               id == OD_PKT_DISPLAY_CAL || id == OD_PKT_DISPLAY_PANEL ||
               id == OD_PKT_ACTION_TABLE;
    }
    return false;          /* legacy packets are stepped over, not indexed (INDEX-FULL) */
}

static bool walk_cb(void *ctx, uint8_t id, od_span_t body)
{
    walk_t *w = ctx;

    if (indexed_type(id) && body.n > 0u && w->n < 64u) {
        walk_pkt_t *p = &w->pkt[w->n++];
        p->type = id;
        p->instance = body.p[0];
        p->sub = body.n >= 2u ? body.p[1] : 0u;
        p->offset = (uint16_t)(body.p - w->base);
        p->len = (uint16_t)body.n;
    }
    return true;
}

/* The index must be exactly the first OD_CFG_INDEX_MAX packets the boot parser delivers. */
static bool index_matches_walk(const uint8_t *payload, uint32_t n)
{
    walk_t w;
    const int count = od_cfg_index_build();

    memset(&w, 0, sizeof w);
    w.base = payload;
    (void)od_config_tlv_walk(od_span_make(payload, n), walk_cb, &w, NULL, NULL);
    const unsigned expect = w.n < OD_CFG_INDEX_MAX ? w.n : OD_CFG_INDEX_MAX;
    if (count < 0 || (unsigned)count != expect) {
        printf("  index %d entries, walk %u\n", count, w.n);
        return false;
    }
    for (unsigned i = 0; i < expect; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry((int)i);
        if (e->type != w.pkt[i].type || e->offset != w.pkt[i].offset ||
            e->len != w.pkt[i].len || e->instance != w.pkt[i].instance ||
            e->sub != w.pkt[i].sub) {
            printf("  entry %u: index %02x@%u+%u, walk %02x@%u+%u\n", i, e->type, e->offset,
                   e->len, w.pkt[i].type, w.pkt[i].offset, w.pkt[i].len);
            return false;
        }
    }
    return true;
}

static uint8_t pad[4096];

/* ------------------------------------------------------------------------------------ cases -- */

static void test_record_framing(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 168u, 384u, 3u);

    CASE("nothing stored: -1, not an empty index");
    od_hal_nvs_erase();
    CHECK(od_cfg_index_build() == -1);
    CHECK(od_cfg_index_count() == 0);
    CHECK(od_cfg_find(OD_PKT_DISPLAY_PANEL, 0u) == NULL);

    CASE("a header-only record is not a config");
    fx_store_payload(pad, 0u);
    CHECK(od_cfg_index_build() == -1);

    CASE("bad magic");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    fx_nvs[0] ^= 0xFFu;
    CHECK(od_cfg_index_build() == -1);

    CASE("declared data_len past the record");
    fx_cfg_store(&c);
    fx_put32(&fx_nvs[12], c.n + 1u);
    CHECK(od_cfg_index_build() == -1);

    CASE("record CRC-32 is NOT checked by the index (od_config_store_load would refuse it)");
    fx_cfg_store(&c);
    fx_nvs[8] ^= 0x01u;
    CHECK(od_cfg_index_build() == 0);
    /* The boot path refuses this record as CORRUPT and boots on defaults, but the index
     * consumers -- the drive path and the action runner -- still read it.
     * od_config_store_save_carry verifies the record itself (CARRY-CORRUPT). */

    CASE("a data_len that agrees with the record is used, not the record size");
    fx_cfg_store(&c);
    fx_nvs_len += 7u;          /* trailing slack in the medium */
    CHECK(od_cfg_index_build() == 0);
    CHECK(od_cfg_blob_len() == c.n);

    CASE("a medium read failure part-way returns what was indexed so far");
    fx_cfg_store(&c);
    fx_nvs_fail_after = 3;     /* size + header read, then the first packet header */
    {
        const int got = od_cfg_index_build();
        CHECK(got >= 0 && got < 4);
    }
}

static void test_legacy_only_2x(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 296u, 128u, 1u);

    CASE("a 2.x config: legacy packets are stepped over, not indexed; no drive data");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(od_cfg_find(0x20u, 0u) == NULL);
    CHECK(od_cfg_find(OD_PKT_DISPLAY_MODE, 0u) == NULL);
    CHECK(!od_cfg_has_drive_data(0u));
    CHECK(index_matches_walk(c.b, c.n));

    CASE("the walk starts after [reserved:2][version], whatever those bytes hold");
    {
        static const uint8_t t[10] = { 10u, 5u, 0, 0, 1u, 1u, 1u, 1u, 16u, 0u };
        fx_cfg_begin(&c, 1u);
        fx_cfg_legacy_head(&c, &d);
        fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, t, sizeof t);
        fx_cfg_finish(&c);
    }
    c.b[0] = 0x12u;            /* the toolbox's length field */
    c.b[1] = 0x34u;
    c.b[2] = 0x07u;
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 1);
}

static void test_real_panel(void)
{
    for (unsigned p = 0; p < FX_PANEL_COUNT; p++) {
        const fx_panel_t *pan = &FX_PANELS[p];
        fx_cfg_t c;
        struct DisplayConfig d = fx_display(0u, pan->width, pan->height, pan->scheme);

        CASE(pan->name);
        fx_cfg_begin(&c, 1u);
        fx_cfg_legacy_head(&c, &d);
        fx_cfg_panel(&c, pan, 0u);
        fx_cfg_finish(&c);
        if (c.n > OD_CONFIG_MAX_SIZE) {
            continue;          /* does not fit this cap; the cap test covers refusal */
        }
        fx_cfg_store(&c);
        CHECK(od_cfg_index_build() == (int)pan->count);
        CHECK(index_matches_walk(c.b, c.n));
        CHECK(od_cfg_has_drive_data(0u) ==
              (od_cfg_find(OD_PKT_DISPLAY_PANEL, 0u) != NULL &&
               od_cfg_find(OD_PKT_DISPLAY_MODE, 0u) != NULL));
        /* Every indexed body reads back byte-identical to what was written. */
        for (unsigned i = 0; i < pan->count; i++) {
            const od_cfg_entry_t *e = od_cfg_index_entry((int)i);
            uint8_t back[1024];
            CHECK(e != NULL && e->type == pan->pkts[i].id && e->len == pan->pkts[i].n);
            if (e != NULL && e->len <= sizeof back) {
                CHECK(od_cfg_read(e, 0u, back, e->len) == e->len);
                CHECK(memcmp(back, pan->pkts[i].body, e->len) == 0);
                CHECK(e->instance == pan->pkts[i].body[0]);
                CHECK(e->len < 2u || e->sub == pan->pkts[i].body[1]);
            }
        }
    }
}

static void test_burned_and_unknown(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 168u, 384u, 3u);
    const fx_panel_t *pan = &FX_PANELS[0];

    CASE("burned 0x2E-0x31 are stepped over at their lab sizes and never indexed");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    memset(pad, 0xAB, sizeof pad);
    fx_cfg_legacy(&c, 0x2Eu, pad, OD_CFG_TLV_BURNED_2E_SIZE);
    fx_cfg_legacy(&c, 0x2Fu, pad, OD_CFG_TLV_BURNED_2F_SIZE);
    fx_cfg_legacy(&c, 0x30u, pad, OD_CFG_TLV_BURNED_30_SIZE);
    fx_cfg_legacy(&c, 0x31u, pad, OD_CFG_TLV_BURNED_31_SIZE);
    fx_cfg_panel(&c, pan, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == (int)pan->count);
    for (int i = 0; i < od_cfg_index_count(); i++) {
        const uint8_t t = od_cfg_index_entry(i)->type;
        CHECK(!(t >= 0x2Eu && t <= 0x31u));
    }
    CHECK(od_cfg_find(OD_PKT_DISPLAY_PANEL, 0u) != NULL);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("an unknown LEGACY id abandons the tail: drive packets after it are lost");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_legacy(&c, 0x3Fu, pad, 8u);
    fx_cfg_panel(&c, pan, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(od_cfg_find(OD_PKT_DISPLAY_PANEL, 0u) == NULL);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("an unknown 0x80+ id is stepped over by its length and costs no slot");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, 0x90u, pad, 10u);
    fx_cfg_ext(&c, 0xFFu, pad, 300u);
    fx_cfg_panel(&c, pan, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == (int)pan->count);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("zero-length drive packets are skipped, not indexed with a zero length");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, NULL, 0u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, NULL, 0u);
    fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, NULL, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(od_cfg_find(OD_PKT_DISPLAY_PANEL, 0u) == NULL);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("IDX-1BYTE: a one-byte body should index its instance (body[0]) with sub = 0");
    fx_cfg_begin(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_LUT, "\x05", 1u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 1);
    CHECK(od_cfg_index_entry(0)->sub == 0u);
    CHECK(od_cfg_index_entry(0)->instance == 5u);
}

static void test_truncation_and_overrun(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 168u, 384u, 3u);
    uint8_t body[40];

    memset(body, 0x11, sizeof body);

    CASE("a declared length one past the CRC tail stops the walk; earlier packets are kept");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext_len(&c, OD_PKT_DISPLAY_MODE, body, sizeof body, (uint16_t)(sizeof body + 1u));
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(od_cfg_find(OD_PKT_DISPLAY_MODE, 0u) == NULL);
    CHECK(index_matches_walk(c.b, c.n));
    /* od_config_parse() reports the same blob TRUNCATED and leaves cfg->loaded false, so the
     * device boots on defaults while the index still serves the four packets in front. */
    CHECK(od_config_tlv_walk(od_span_make(c.b, c.n), walk_cb, &(walk_t){ .base = c.b }, NULL,
                             NULL) == OD_CFG_TLV_TRUNCATED);

    CASE("a body ending exactly at the CRC tail is accepted");
    fx_cfg_begin(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, body, sizeof body);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 1);
    CHECK(od_cfg_index_entry(0)->len == sizeof body);

    CASE("length 0xFFFF does not wrap the offset arithmetic");
    fx_cfg_begin(&c, 1u);
    fx_cfg_ext_len(&c, OD_PKT_DISPLAY_LUT, body, sizeof body, 0xFFFFu);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("a length-prefixed header that straddles the CRC tail");
    fx_cfg_begin(&c, 1u);
    fx_cfg_u8(&c, 0u);
    fx_cfg_u8(&c, OD_PKT_DISPLAY_PANEL);
    fx_cfg_u8(&c, 3u);          /* only one length byte before the CRC */
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("a legacy body running into the CRC tail");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy(&c, 0x20u, &d, sizeof d - 1u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("one stray byte before the CRC is ignored by both walkers");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_u8(&c, 0x5Au);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 0);
    CHECK(index_matches_walk(c.b, c.n));

    CASE("blobs of 0..6 bytes");
    for (uint32_t n = 0; n < 7u; n++) {
        memset(pad, 0, n);
        fx_store_payload(pad, n);
        const int got = od_cfg_index_build();
        CHECK(got == (n == 0u ? -1 : 0));
    }
}

static void test_duplicates(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 168u, 384u, 3u);
    uint8_t t1[10] = { 6u, 1u, 0, 0, 1u, 1u, 1u, 1u, 16u, 0u };   /* one BOOT entry */
    uint8_t t2[10] = { 6u, 5u, 0, 0, 1u, 1u, 1u, 2u, 16u, 0u };   /* one HOST entry */

    CASE("duplicate 0xB2: od_cfg_find(…, 0) is the FIRST (older) table -- the documented trap");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, t1, sizeof t1);
    fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, t2, sizeof t2);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 2);
    {
        const od_cfg_entry_t *a = od_cfg_find(OD_PKT_ACTION_TABLE, 0u);
        const od_cfg_entry_t *b = od_cfg_find(OD_PKT_ACTION_TABLE, 1u);
        uint8_t got[2];
        CHECK(a != NULL && b != NULL && a->offset < b->offset);
        CHECK(a != NULL && od_cfg_read(a, 0u, got, 2u) == 2u && got[1] == 1u);
        CHECK(od_cfg_find(OD_PKT_ACTION_TABLE, 2u) == NULL);
    }
    CHECK(index_matches_walk(c.b, c.n));

    CASE("od_cfg_find's second argument is an ORDINAL, not an instance number");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_panel(&c, &FX_PANELS[0], 1u);   /* only display 1 has drive data */
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() > 0);
    CHECK(od_cfg_find(OD_PKT_DISPLAY_PANEL, 0u) != NULL);     /* instance 1's panel */
    CHECK(od_cfg_find(OD_PKT_DISPLAY_PANEL, 1u) == NULL);     /* asking for "instance 1" */
    CHECK(od_cfg_has_drive_data(1u));
    CHECK(!od_cfg_has_drive_data(0u));
}

static void test_two_displays(void)
{
    fx_cfg_t c;
    struct DisplayConfig d0 = fx_display(0u, 400u, 300u, 0u);
    struct DisplayConfig d1 = fx_display(1u, 400u, 300u, 0u);

    CASE("drive packets for two displays index with their own instances");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d0);
    fx_cfg_legacy(&c, 0x20u, &d1, sizeof d1);
    fx_cfg_panel(&c, &FX_PANELS[0], 0u);
    fx_cfg_panel(&c, &FX_PANELS[1], 1u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == (int)(FX_PANELS[0].count + FX_PANELS[1].count));
    CHECK(od_cfg_has_drive_data(0u) && od_cfg_has_drive_data(1u));
    CHECK(index_matches_walk(c.b, c.n));
}

static void test_caps(void)
{
    fx_cfg_t c;
    const uint32_t cap = OD_CONFIG_MAX_SIZE;
    struct DisplayConfig d = fx_display(0u, 168u, 384u, 3u);

    CASE("a config of exactly OD_CONFIG_MAX_SIZE indexes, the drive packets at the very end");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    {
        /* Filler: an unknown 0x80+ packet sized so the whole blob lands on the cap. */
        const uint32_t tail = 4u + FX_PANELS[0].pkts[0].n + 4u + FX_PANELS[0].pkts[1].n + 2u;
        const uint32_t fill = cap - c.n - 4u - tail;
        memset(pad, 0xEE, sizeof pad);
        fx_cfg_ext(&c, 0xC0u, pad, fill);
    }
    fx_cfg_ext(&c, FX_PANELS[0].pkts[0].id, FX_PANELS[0].pkts[0].body, FX_PANELS[0].pkts[0].n);
    fx_cfg_ext(&c, FX_PANELS[0].pkts[1].id, FX_PANELS[0].pkts[1].body, FX_PANELS[0].pkts[1].n);
    fx_cfg_finish(&c);
    CHECK(c.n == cap);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 2);
    CHECK(od_cfg_has_drive_data(0u));
    CHECK(index_matches_walk(c.b, c.n));

    CASE("one byte over the cap is refused outright");
    fx_store_payload(c.b, c.n);
    {
        uint8_t big[4097 + 8];
        memcpy(big, c.b, c.n);
        big[c.n] = 0u;
        fx_store_payload(big, c.n + 1u);
    }
    CHECK(od_cfg_index_build() == -1);
}

static void test_index_full(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 168u, 384u, 3u);
    struct LedConfig led;
    unsigned legacy = 0u;

    memset(&led, 0, sizeof led);
    CASE("INDEX-FULL: 28 legacy packets cost no index slot; the panel's drive packets all index");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    legacy = 4u;
    while (legacy < 28u) {
        fx_cfg_legacy(&c, 0x21u, &led, sizeof led);
        legacy++;
    }
    fx_cfg_panel(&c, &FX_PANELS[3], 0u);   /* 6 packets: 34 in all */
    fx_cfg_finish(&c);
    CHECK(c.n <= 4096u);
    if (c.n <= OD_CONFIG_MAX_SIZE) {
        fx_cfg_store(&c);
        CHECK(od_cfg_index_build() == (int)FX_PANELS[3].count);
        CHECK(index_matches_walk(c.b, c.n));
        /* A legal config (each legacy type is capped at 4 instances by od_config, but there
         * are ten repeatable types). Legacy packets, which nothing reads through the index,
         * used to spend the 32 slots first and leave the panel without LUT slot 1 and its
         * colour16 mode. */
        unsigned drive = 0u;
        for (int i = 0; i < od_cfg_index_count(); i++) {
            drive += od_cfg_index_entry(i)->type >= 0x80u ? 1u : 0u;
        }
        CHECK(drive == FX_PANELS[3].count);
    }
}

static void test_cfg_read(void)
{
    fx_cfg_t c;
    uint8_t body[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    uint8_t out[16];

    CASE("od_cfg_read clamps to the body and refuses an offset at or past its end");
    fx_cfg_begin(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, body, sizeof body);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_index_build() == 1);
    const od_cfg_entry_t *e = od_cfg_index_entry(0);
    CHECK(od_cfg_read(e, 6u, out, 10u) == 2u && out[0] == 6u && out[1] == 7u);
    CHECK(od_cfg_read(e, 8u, out, 1u) == 0u);
    CHECK(od_cfg_read(NULL, 0u, out, 1u) == 0u);
    CHECK(od_cfg_read(e, 0u, NULL, 1u) == 0u);
    CHECK(od_cfg_index_entry(-1) == NULL && od_cfg_index_entry(1) == NULL);

    CASE("entries go stale after a config write (documented): same offset, new bytes");
    body[0] = 9u;
    fx_cfg_begin(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, body, sizeof body);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    CHECK(od_cfg_read(e, 0u, out, 1u) == 1u && out[0] == 9u);
}

int main(void)
{
    test_record_framing();
    test_legacy_only_2x();
    test_real_panel();
    test_burned_and_unknown();
    test_truncation_and_overrun();
    test_duplicates();
    test_two_displays();
    test_caps();
    test_index_full();
    test_cfg_read();
    printf("cfg_index (cap %u): %u known bugs\n", (unsigned)OD_CONFIG_MAX_SIZE, fx_xfail_count);
    return OD_CHECK_REPORT_NONEMPTY("cfg_index", 100);
}
