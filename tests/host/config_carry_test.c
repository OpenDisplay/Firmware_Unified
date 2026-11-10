/* od_config_carry -- drive packets survive a config write that has none, over a fake medium.
 *
 * Compiled against OD_CONFIG_MAX_SIZE 4096 and 2048 (shared/profiles.cmake) like
 * config_store_test, because the cap bounds what a merge may grow to.
 */

#include "od_cfg_index.h"
#include "od_config_carry.h"
#include "od_config_store.h"
#include "od_config_tlv.h"
#include "od_hal_nvs.h"

#include "od_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    (void)level;
    va_start(ap, fmt);
    va_end(ap);
}

static uint8_t  g_medium[OD_CONFIG_STORE_MAX_RECORD];
static uint32_t g_medium_len;
static bool     g_present;
static unsigned g_writes;

int od_hal_nvs_init(void) { return OD_HAL_NVS_OK; }

int od_hal_nvs_size(uint32_t *len_out)
{
    *len_out = g_present ? g_medium_len : 0u;
    return g_present ? OD_HAL_NVS_OK : OD_HAL_NVS_ENOENT;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    if (!g_present) { return OD_HAL_NVS_ENOENT; }
    if (offset > g_medium_len || len > g_medium_len - offset) { return OD_HAL_NVS_E2BIG; }
    memcpy(buf, g_medium + offset, len);
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
    if (len > sizeof(g_medium)) { return OD_HAL_NVS_E2BIG; }
    memcpy(g_medium, record, len);
    g_medium_len = len;
    g_present = true;
    g_writes++;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_erase(void) { g_present = false; g_medium_len = 0u; return OD_HAL_NVS_OK; }

/* ------------------------------------------------------------------ blob builder -------- */

typedef struct { uint8_t b[OD_CONFIG_MAX_SIZE]; uint32_t n; } blob_t;

static void blob_begin(blob_t *x)
{
    x->b[0] = 0u; x->b[1] = 0u; x->b[2] = 1u;
    x->n = 3u;
}

static void blob_legacy(blob_t *x, uint8_t id, uint8_t fill)
{
    const uint16_t sz = od_config_tlv_body_size(id);

    x->b[x->n++] = 0u;
    x->b[x->n++] = id;
    memset(x->b + x->n, fill, sz);
    x->b[x->n] = 0u;                 /* instance 0 */
    x->n += sz;
}

static void blob_lp(blob_t *x, uint8_t id, uint16_t len, uint8_t fill)
{
    x->b[x->n++] = 7u;
    x->b[x->n++] = id;
    x->b[x->n++] = (uint8_t)(len & 0xFFu);
    x->b[x->n++] = (uint8_t)(len >> 8);
    memset(x->b + x->n, fill, len);
    if (len > 0u) { x->b[x->n] = 0u; }
    x->n += len;
}

static void blob_end(blob_t *x, bool good_crc)
{
    uint16_t c = od_config_tlv_crc16(od_span_make(x->b, x->n));

    if (!good_crc) { c ^= 0x5A5Au; }
    x->b[x->n++] = (uint8_t)(c & 0xFFu);
    x->b[x->n++] = (uint8_t)(c >> 8);
}

static uint8_t g_ws[OD_CONFIG_STORE_MAX_RECORD];

static void store_raw(const blob_t *x)
{
    g_present = false;
    CHECK(od_config_store_save(g_ws, sizeof(g_ws), x->b, x->n) == OD_CONFIG_STORE_OK);
}

static const uint8_t *stored_payload(uint32_t *len)
{
    *len = (uint32_t)g_medium[12] | ((uint32_t)g_medium[13] << 8);
    return g_medium + OD_CONFIG_STORE_HEADER_SIZE;
}

static bool stored_crc_ok(void)
{
    uint32_t n;
    const uint8_t *p = stored_payload(&n);

    return od_config_tlv_crc16(od_span_make(p, n - 2u)) ==
           (uint16_t)(p[n - 2u] | (p[n - 1u] << 8));
}

/* The packets the cases use. 0x01 is a legacy packet every build knows. */
#define LEGACY 0x01u

static void stored_with_drive(void)
{
    blob_t s;

    blob_begin(&s);
    blob_legacy(&s, LEGACY, 0x11u);
    blob_lp(&s, OD_PKT_DISPLAY_PANEL, 20u, 0xB1u);
    blob_lp(&s, OD_PKT_DISPLAY_MODE, 40u, 0xAEu);
    blob_lp(&s, OD_PKT_DISPLAY_MODE, 30u, 0xAFu);
    blob_end(&s, true);
    store_raw(&s);
}

int main(void)
{
    blob_t in;
    uint8_t carried;
    uint32_t n;
    const uint8_t *p;

    CHECK(od_config_tlv_body_size(LEGACY) > 0u);

    CASE("legacy-only write keeps the stored drive packets, in order");
    stored_with_drive();
    blob_begin(&in);
    blob_legacy(&in, LEGACY, 0x22u);
    blob_end(&in, true);
    CHECK(!od_config_has_extended_packets(in.b, in.n));
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
          == OD_CONFIG_STORE_OK);
    CHECK(carried == 3u);
    p = stored_payload(&n);
    CHECK(n == in.n + (4u + 20u) + (4u + 40u) + (4u + 30u));
    CHECK(memcmp(p, in.b, in.n - 2u) == 0);                 /* new legacy bytes, unchanged */
    CHECK(p[in.n - 2u + 1u] == OD_PKT_DISPLAY_PANEL);
    CHECK(p[in.n - 2u + 24u + 1u] == OD_PKT_DISPLAY_MODE);
    CHECK(p[in.n - 2u + 24u + 4u + 1u] == 0xAEu);
    CHECK(stored_crc_ok());
    CHECK(od_cfg_has_drive_data(0u));
    CHECK(od_cfg_find(OD_PKT_DISPLAY_MODE, 1u) != NULL &&
          od_cfg_find(OD_PKT_DISPLAY_MODE, 1u)->len == 30u);

    CASE("payload already in the workspace slot (the BG22 overlay)");
    stored_with_drive();
    memcpy(od_config_store_payload(g_ws), in.b, in.n);
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), od_config_store_payload(g_ws), in.n,
                                     &carried) == OD_CONFIG_STORE_OK);
    CHECK(carried == 3u);
    p = stored_payload(&n);
    CHECK(n == in.n + 102u && stored_crc_ok());

    CASE("an incoming CRC mismatch stays a mismatch");
    stored_with_drive();
    blob_begin(&in);
    blob_legacy(&in, LEGACY, 0x22u);
    blob_end(&in, false);
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
          == OD_CONFIG_STORE_OK);
    CHECK(carried == 3u && !stored_crc_ok());

    CASE("a zero-length 0xB1 is the explicit wipe: stored as sent");
    stored_with_drive();
    blob_begin(&in);
    blob_legacy(&in, LEGACY, 0x22u);
    blob_lp(&in, OD_PKT_DISPLAY_PANEL, 0u, 0u);
    blob_end(&in, true);
    CHECK(od_config_has_extended_packets(in.b, in.n));
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
          == OD_CONFIG_STORE_OK);
    CHECK(carried == 0u);
    p = stored_payload(&n);
    CHECK(n == in.n && memcmp(p, in.b, n) == 0);
    CHECK(!od_cfg_has_drive_data(0u));

    CASE("a 3.x config with its own drive packets replaces the stored ones");
    stored_with_drive();
    blob_begin(&in);
    blob_legacy(&in, LEGACY, 0x22u);
    blob_lp(&in, OD_PKT_DISPLAY_PANEL, 8u, 0x33u);
    blob_lp(&in, OD_PKT_DISPLAY_MODE, 8u, 0x44u);
    blob_end(&in, true);
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
          == OD_CONFIG_STORE_OK);
    p = stored_payload(&n);
    CHECK(carried == 0u && n == in.n && memcmp(p, in.b, n) == 0);

    CASE("any other 0x80+ packet also opts out");
    stored_with_drive();
    blob_begin(&in);
    blob_legacy(&in, LEGACY, 0x22u);
    blob_lp(&in, 0xF0u, 3u, 0x55u);
    blob_end(&in, true);
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
          == OD_CONFIG_STORE_OK);
    p = stored_payload(&n);
    CHECK(carried == 0u && n == in.n);

    CASE("nothing stored, or a stored config without drive packets: stored as sent");
    g_present = false;
    blob_begin(&in);
    blob_legacy(&in, LEGACY, 0x22u);
    blob_end(&in, true);
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
          == OD_CONFIG_STORE_OK);
    p = stored_payload(&n);
    CHECK(carried == 0u && n == in.n && memcmp(p, in.b, n) == 0);
    {
        blob_t s;

        blob_begin(&s);
        blob_legacy(&s, LEGACY, 0x11u);
        blob_end(&s, true);
        store_raw(&s);
    }
    CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
          == OD_CONFIG_STORE_OK);
    p = stored_payload(&n);
    CHECK(carried == 0u && n == in.n);

    CASE("a merge past the cap is refused and nothing is written");
    {
        blob_t s;
        const uint16_t big = (uint16_t)(OD_CONFIG_MAX_SIZE / 2u);
        unsigned writes;
        uint32_t before;

        blob_begin(&s);
        blob_lp(&s, OD_PKT_DISPLAY_PANEL, 8u, 0xB1u);
        blob_lp(&s, OD_PKT_DISPLAY_MODE, big, 0xAEu);
        blob_end(&s, true);
        store_raw(&s);
        before = g_medium_len;
        writes = g_writes;
        blob_begin(&in);
        while (in.n + od_config_tlv_body_size(LEGACY) + 2u + 2u < big + 64u) {
            blob_legacy(&in, LEGACY, 0x22u);
        }
        blob_end(&in, true);
        CHECK(od_config_store_save_carry(g_ws, sizeof(g_ws), in.b, in.n, &carried)
              == OD_CONFIG_STORE_TOO_BIG);
        CHECK(carried == 0u && g_writes == writes && g_medium_len == before);
        (void)od_cfg_index_build();
        CHECK(od_cfg_find(OD_PKT_DISPLAY_MODE, 0u) != NULL &&
              od_cfg_find(OD_PKT_DISPLAY_MODE, 0u)->len == big);
    }

    return OD_CHECK_REPORT_NONEMPTY("config_carry", 40);
}
