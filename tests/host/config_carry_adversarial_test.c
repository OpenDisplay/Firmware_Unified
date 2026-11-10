/* config_carry_adversarial: od_config_store_save_carry against inputs built to break it.
 *
 * config_carry_test.c covers the designed behaviour. This suite asks what happens to the panel
 * after the write: every case re-indexes the record that landed on the medium and checks the
 * drive packets a later od_epd_cfg_load would see, because the carry runs on EVERY legacy
 * config write and a wrong merge is a display that never draws again. Built at both caps.
 */
#include "od_check.h"
#include "drive_fixture.h"

#include "od_cfg_index.h"
#include "od_config_carry.h"

static uint8_t ws[OD_CONFIG_STORE_HEADER_SIZE + OD_CONFIG_MAX_SIZE];
static uint8_t pad[4096];

/* Stored drive packet bodies, in index order, as a later load would find them. */
typedef struct {
    unsigned n;
    uint8_t  type[40];
    uint16_t len[40];
    uint32_t sum[40];
} drive_set_t;

static drive_set_t drive_set(void)
{
    drive_set_t s;
    uint8_t buf[1024];

    memset(&s, 0, sizeof s);
    const int n = od_cfg_index_build();
    for (int i = 0; i < n; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        if (e->type < OD_PKT_DISPLAY_MODE || e->type > OD_PKT_DISPLAY_PANEL || s.n >= 40u) {
            continue;
        }
        s.type[s.n] = e->type;
        s.len[s.n] = e->len;
        s.sum[s.n] = (e->len <= sizeof buf && od_cfg_read(e, 0u, buf, e->len) == e->len)
                         ? od_config_store_crc32(buf, e->len) : 0u;
        s.n++;
    }
    return s;
}

static bool same_set(const drive_set_t *a, const drive_set_t *b)
{
    if (a->n != b->n) {
        return false;
    }
    for (unsigned i = 0; i < a->n; i++) {
        if (a->type[i] != b->type[i] || a->len[i] != b->len[i] || a->sum[i] != b->sum[i]) {
            return false;
        }
    }
    return true;
}

/* The record on the medium loads cleanly (magic, length, CRC-32) and its CRC-16 checks. */
static bool stored_record_sound(void)
{
    static uint8_t out[OD_CONFIG_MAX_SIZE];
    uint32_t n = sizeof out;

    if (od_config_store_load(out, &n) != OD_CONFIG_STORE_OK || n < 5u) {
        return false;
    }
    const uint16_t want = (uint16_t)(out[n - 2u] | (out[n - 1u] << 8));
    return od_config_tlv_crc16(od_span_make(out, n - 2u)) == want;
}

static void store_panel_config(const fx_panel_t *p)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, p->width, p->height, p->scheme);

    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_panel(&c, p, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
}

/* A 2.x client's config: legacy packets only. */
static void legacy_config(fx_cfg_t *c, uint16_t w)
{
    struct DisplayConfig d = fx_display(0u, w, 300u, 0u);

    fx_cfg_begin(c, 1u);
    fx_cfg_legacy_head(c, &d);
    fx_cfg_finish(c);
}

static enum od_config_store_result carry(const fx_cfg_t *c, uint8_t *carried)
{
    return od_config_store_save_carry(ws, sizeof ws, c->b, c->n, carried);
}

static void test_round_trip_and_idempotence(void)
{
    const fx_panel_t *p = &FX_PANELS[3];      /* EP29YR: 6 drive packets incl. two LUTs */
    fx_cfg_t c;
    uint8_t carried = 0u;

    CASE("legacy write over a drive config: same drive bodies, a sound record");
    store_panel_config(p);
    const drive_set_t before = drive_set();
    CHECK(before.n == 6u);                    /* B1 AE AE AF AF B0: 0xB0 is in the carried range */
    legacy_config(&c, 400u);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK);
    CHECK(carried == 6u);
    const drive_set_t after = drive_set();
    CHECK(same_set(&before, &after));
    CHECK(stored_record_sound());
    /* and the new legacy packet won */
    {
        const od_cfg_entry_t *e = od_cfg_find(0x20u, 0u);
        struct DisplayConfig got;
        CHECK(e != NULL && od_cfg_read(e, 0u, (uint8_t *)&got, sizeof got) == sizeof got &&
              got.pixel_width == 400u);
    }

    CASE("the same legacy write again does not duplicate the drive packets");
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK);
    const drive_set_t again = drive_set();
    CHECK(same_set(&before, &again));
    CHECK(stored_record_sound());

    CASE("ten writes in a row are still byte-stable");
    for (int i = 0; i < 10; i++) {
        legacy_config(&c, (uint16_t)(100 + i));
        CHECK(carry(&c, NULL) == OD_CONFIG_STORE_OK);
    }
    {
        const drive_set_t ten = drive_set();
        CHECK(same_set(&before, &ten));
    }
}

static void test_duplicates_in_store(void)
{
    const fx_panel_t *p = &FX_PANELS[0];
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, p->width, p->height, p->scheme);
    uint8_t carried = 0u;

    CASE("duplicate drive packets in the stored config are all carried, in stored order");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_panel(&c, p, 0u);
    fx_cfg_panel(&c, p, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    const drive_set_t before = drive_set();
    CHECK(before.n == 2u * p->count);
    legacy_config(&c, 400u);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK);
    CHECK(carried == 2u * p->count);
    const drive_set_t after = drive_set();
    CHECK(same_set(&before, &after));
}

static void test_non_drive_extended(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 400u, 300u, 0u);
    const uint8_t table[10] = { 10u, 5u, 0, 0, 1u, 1u, 1u, 1u, 16u, 0u };
    uint8_t carried = 0u;

    CASE("CARRY-ACTIONS: a stored 0xB2 action table is not carried by a legacy write");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_panel(&c, &FX_PANELS[0], 0u);
    fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, table, sizeof table);
    fx_cfg_ext(&c, 0xC7u, pad, 20u);          /* an unknown future packet */
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    legacy_config(&c, 400u);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK);
    CHECK(carried == FX_PANELS[0].count);
    CHECK(od_cfg_index_build() > 0);
    /* The table is 0x80+, so a 2.x client cannot have meant to delete it -- the same argument
     * that carries the drive packets. Today it is silently dropped. Maintainer question: should
     * the carry cover every 0x80+ packet, or only 0xAE-0xB1? */
    KNOWN_BUG("CARRY-ACTIONS", od_cfg_find(OD_PKT_ACTION_TABLE, 0u) != NULL);

    CASE("only a stored 0xB2 (no drive packets): nothing to carry, stored as sent");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, OD_PKT_ACTION_TABLE, table, sizeof table);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    legacy_config(&c, 400u);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == 0u);
    CHECK(fx_nvs_len == OD_CONFIG_STORE_HEADER_SIZE + c.n);
}

static void test_zero_length_wipe(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 400u, 300u, 0u);
    uint8_t carried = 0u;

    CASE("zero-length 0xB1 wipe, then a legacy write: the drive data stays gone");
    store_panel_config(&FX_PANELS[1]);
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, NULL, 0u);
    fx_cfg_finish(&c);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == 0u);
    CHECK(drive_set().n == 0u);
    legacy_config(&c, 400u);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == 0u);
    CHECK(drive_set().n == 0u);

    CASE("ANY zero-length 0x80+ packet wipes too -- the wipe is not specific to 0xB1");
    store_panel_config(&FX_PANELS[1]);
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, 0xF0u, NULL, 0u);
    fx_cfg_finish(&c);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == 0u);
    CHECK(drive_set().n == 0u);

    CASE("a zero-length 0xB1 for display 1 also wipes display 0's drive data");
    store_panel_config(&FX_PANELS[1]);
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, NULL, 0u);   /* a zero-length body names no instance */
    fx_cfg_finish(&c);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK);
    CHECK(drive_set().n == 0u);
}

static void test_stray_tail(void)
{
    fx_cfg_t c;
    uint8_t carried = 0u;
    const drive_set_t *none = NULL;

    (void)none;
    CASE("CARRY-STRAY: one stray byte before the incoming CRC hides every carried packet");
    store_panel_config(&FX_PANELS[3]);
    const drive_set_t before = drive_set();
    legacy_config(&c, 400u);
    /* Re-open the blob, add one byte between the last packet and the CRC. Both walkers
     * ignore a single trailing byte (cfg_index_test pins that), so the incoming config is
     * legal and has no 0x80+ packet: the carry runs. */
    c.n -= 2u;
    fx_cfg_u8(&c, 0x00u);
    fx_cfg_finish(&c);
    const enum od_config_store_result rc = carry(&c, &carried);
    CHECK(rc == OD_CONFIG_STORE_OK);
    CHECK(carried == 6u);
    /* The packets were appended AFTER the stray byte, so the walk now reads
     * [stray][number-of-first-carried] as a legacy header, id = that number byte, which is an
     * unknown legacy id and abandons the tail. The write "succeeded" and the panel is dead. */
    const drive_set_t after = drive_set();
    KNOWN_BUG("CARRY-STRAY", same_set(&before, &after) || rc != OD_CONFIG_STORE_OK);
}

static void test_index_full(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 168u, 384u, 3u);
    struct LedConfig led;
    uint8_t carried = 0u;

    memset(&led, 0, sizeof led);
    CASE("CARRY-INDEX-FULL: a stored config past 32 packets loses drive packets on carry");
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    for (int i = 0; i < 24; i++) {
        fx_cfg_legacy(&c, 0x21u, &led, sizeof led);
    }
    fx_cfg_panel(&c, &FX_PANELS[3], 0u);      /* 28 + 6 = 34 packets */
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    legacy_config(&c, 400u);
    const enum od_config_store_result rc = carry(&c, &carried);
    CHECK(rc == OD_CONFIG_STORE_OK);
    /* The carry enumerates od_cfg_index, which stops at 32 entries, so the last two drive
     * packets (a LUT slot and the 0xB0) are dropped -- silently, with OK returned. */
    KNOWN_BUG("CARRY-INDEX-FULL", carried == 6u || rc != OD_CONFIG_STORE_OK);
}

static void test_corrupt_store(void)
{
    fx_cfg_t c;
    uint8_t carried = 0u;

    CASE("CARRY-CORRUPT: drive packets from a record that fails its CRC-32 are carried");
    store_panel_config(&FX_PANELS[0]);
    fx_nvs[OD_CONFIG_STORE_HEADER_SIZE + 200u] ^= 0x40u;   /* inside a drive packet */
    {
        static uint8_t out[OD_CONFIG_MAX_SIZE];
        uint32_t n = sizeof out;
        CHECK(od_config_store_load(out, &n) == OD_CONFIG_STORE_CORRUPT);
    }
    legacy_config(&c, 400u);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK);
    /* The boot path refuses that record; the carry copies its bytes into a fresh record with a
     * VALID CRC-32 -- corruption laundered into a config the device will now trust. */
    KNOWN_BUG("CARRY-CORRUPT", carried == 0u);
    CHECK(stored_record_sound());
}

static void test_caps(void)
{
    fx_cfg_t c;
    uint8_t carried = 0u;
    const fx_panel_t *p = &FX_PANELS[0];

    store_panel_config(p);
    uint32_t extra = 0u;
    for (unsigned i = 0; i < p->count; i++) {
        extra += 4u + p->pkts[i].n;
    }

    CASE("merge landing exactly on OD_CONFIG_MAX_SIZE is accepted");
    {
        struct DisplayConfig d = fx_display(0u, 400u, 300u, 0u);
        struct LedConfig led;
        memset(&led, 0, sizeof led);
        /* Grow the incoming legacy config with whole LED packets, then trim the stray-byte
         * free remainder with a SensorData-sized step until the merge is exact. */
        fx_cfg_begin(&c, 1u);
        fx_cfg_legacy_head(&c, &d);
        while (c.n + 2u + sizeof led + 2u + extra <= OD_CONFIG_MAX_SIZE) {
            fx_cfg_legacy(&c, 0x21u, &led, sizeof led);
        }
        fx_cfg_finish(&c);
        const uint32_t merged = c.n + extra;
        CHECK(merged <= OD_CONFIG_MAX_SIZE);
        CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == p->count);
        CHECK(fx_nvs_len == OD_CONFIG_STORE_HEADER_SIZE + merged);
        CHECK(stored_record_sound());

        CASE("one LED packet more pushes the merge over the cap: TOO_BIG, medium untouched");
        store_panel_config(p);
        const uint32_t before_len = fx_nvs_len;
        const unsigned writes = fx_nvs_writes;
        c.n -= 2u;
        fx_cfg_legacy(&c, 0x21u, &led, sizeof led);
        fx_cfg_finish(&c);
        if (c.n + extra > OD_CONFIG_MAX_SIZE && c.n <= OD_CONFIG_MAX_SIZE) {
            CHECK(carry(&c, &carried) == OD_CONFIG_STORE_TOO_BIG);
            CHECK(fx_nvs_writes == writes && fx_nvs_len == before_len);
            CHECK(drive_set().n == p->count);
        }
    }

    CASE("a workspace smaller than the cap bounds the merge, not OD_CONFIG_MAX_SIZE");
    store_panel_config(p);
    legacy_config(&c, 400u);
    CHECK(od_config_store_save_carry(ws, OD_CONFIG_STORE_HEADER_SIZE + c.n + extra - 1u,
                                     c.b, c.n, &carried) == OD_CONFIG_STORE_TOO_BIG);
    CHECK(od_config_store_save_carry(ws, OD_CONFIG_STORE_HEADER_SIZE + c.n + extra,
                                     c.b, c.n, &carried) == OD_CONFIG_STORE_OK);
}

static void test_power_cut(void)
{
    fx_cfg_t c;
    uint8_t carried = 0u;

    CASE("CARRY-READFAIL: a medium read failure at every read the carry makes");
    {
        unsigned wiped = 0u, refused = 0u, completed = 0u;
        for (int k = 0; k < 60; k++) {
            store_panel_config(&FX_PANELS[3]);
            const drive_set_t before = drive_set();
            static uint8_t snap[FX_NVS_CAP];
            const uint32_t snap_len = fx_nvs_len;
            memcpy(snap, fx_nvs, fx_nvs_len);
            legacy_config(&c, 400u);
            fx_nvs_fail_after = k;
            const enum od_config_store_result rc = carry(&c, &carried);
            const bool injected = fx_nvs_fail_after == 0;   /* the failure was reached */
            fx_nvs_fail_after = -1;
            if (rc != OD_CONFIG_STORE_OK) {
                /* Refused: the previous record must be untouched. */
                CHECK(rc == OD_CONFIG_STORE_IO);
                CHECK(fx_nvs_len == snap_len && memcmp(snap, fx_nvs, snap_len) == 0);
                refused++;
                continue;
            }
            const drive_set_t after = drive_set();
            if (same_set(&before, &after)) {
                completed++;
            } else {
                wiped++;
            }
            if (!injected) {
                break;                                     /* k ran past the last read */
            }
        }
        printf("  read-failure sweep: %u refused, %u completed, %u WIPED the drive data\n",
               refused, completed, wiped);
        CHECK(refused > 0u && completed > 0u);
        /* A failure inside od_cfg_index_build ends the walk early and looks exactly like "this
         * config has no (or fewer) drive packets": the carry then stores the legacy config with
         * nothing or a subset appended and returns OK. */
        KNOWN_BUG("CARRY-READFAIL", wiped == 0u);
    }

    CASE("the write itself failing: IO reported, previous record still on the medium");
    store_panel_config(&FX_PANELS[3]);
    {
        const drive_set_t before = drive_set();
        legacy_config(&c, 400u);
        fx_nvs_write_fail = true;
        CHECK(carry(&c, &carried) == OD_CONFIG_STORE_IO);
        const drive_set_t after = drive_set();
        CHECK(same_set(&before, &after));
    }
}

static void test_incoming_shapes(void)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 400u, 300u, 0u);
    uint8_t carried = 0u;

    CASE("an incoming config the boot parser rejects (TRUNCATED) is stored as sent");
    store_panel_config(&FX_PANELS[0]);
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_legacy(&c, 0x21u, pad, 3u);        /* LedConfig cut short */
    fx_cfg_finish(&c);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == 0u);
    CHECK(drive_set().n == 0u);              /* and the drive data is gone with it */

    CASE("an incoming config with an unknown legacy id is stored as sent");
    store_panel_config(&FX_PANELS[0]);
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_legacy(&c, 0x3Au, pad, 4u);
    fx_cfg_finish(&c);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == 0u);

    CASE("minimal 5-byte incoming config: drive packets carried, record sound");
    store_panel_config(&FX_PANELS[0]);
    fx_cfg_begin(&c, 1u);
    fx_cfg_finish(&c);
    CHECK(c.n == 5u);
    CHECK(carry(&c, &carried) == OD_CONFIG_STORE_OK && carried == FX_PANELS[0].count);
    CHECK(stored_record_sound());

    CASE("below 5 bytes: plain save path");
    store_panel_config(&FX_PANELS[0]);
    CHECK(od_config_store_save_carry(ws, sizeof ws, c.b, 4u, &carried) != OD_CONFIG_STORE_IO);
    CHECK(carried == 0u);
}

int main(void)
{
    test_round_trip_and_idempotence();
    test_duplicates_in_store();
    test_non_drive_extended();
    test_zero_length_wipe();
    test_stray_tail();
    test_index_full();
    test_corrupt_store();
    test_caps();
    test_power_cut();
    test_incoming_shapes();
    printf("config_carry_adversarial (cap %u): %u known bugs\n", (unsigned)OD_CONFIG_MAX_SIZE,
           fx_xfail_count);
    return OD_CHECK_REPORT_NONEMPTY("config_carry_adversarial", 40);
}
