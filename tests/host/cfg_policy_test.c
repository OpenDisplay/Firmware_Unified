/* cfg_policy_test.c -- the SEC2 config policy (od_cfg_policy.c) over the real od_sec2 state, the
 * real od_config_store and an in-memory medium.
 *
 * Two halves: py-opendisplay's independent vectors (tests/host/fixtures/sec2_vectors.json, V1 V10 V11
 * V13; any disagreement is a spec question, not a test to adjust), and the branches the vectors do
 * not reach (LOCKED, the scrub, the stale-key zeroing, unwalkable blobs, the marker an open writer
 * must not plant). */
#include "od_boot_payload.h"
#include "od_cfg_policy.h"
#include "od_config_store.h"
#include "od_config_tlv.h"
#include "od_hal_nvs.h"
#include "od_hal_radio.h"
#include "od_txq.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_sec2_kdf.h"
#include "od_sec2_wire.h"
#include "session_fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_checks, g_failures;
static const char *g_case = "";
#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            ++g_failures;                                                                    \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);               \
        }                                                                                    \
    } while (0)
#define CASE(name) (g_case = (name))

struct v1 { const char *k, *label, *ctx, *out; };
struct v8 { const char *raw, *code; };
struct v10 { const char *name, *device, *blob; int verdict; const char *stored, *k_dev; int chunked; };
struct v11 { const char *name, *k_dev, *uid, *before, *written, *after, *read; bool clear; };
struct v13 { char k; uint32_t i; int t, ma, mi, pa, b, d, p; const char *url; };
#include "sec2_vectors.inc"

#define N(a) (sizeof(a) / sizeof((a)[0]))

static size_t unhex(const char *h, uint8_t *out, size_t cap)
{
    size_t n = strlen(h) / 2u;

    if (n > cap) {
        abort();
    }
    for (size_t i = 0; i < n; ++i) {
        unsigned b;
        sscanf(h + 2u * i, "%2x", &b);
        out[i] = (uint8_t)b;
    }
    return n;
}

/* ------------------------------------------------------------------------ the medium --- */

static uint8_t  g_rec[OD_CONFIG_STORE_MAX_RECORD];
static uint32_t g_rec_len;
static bool     g_rec_present;
static int      g_nvs_fail;    /* writes left before a failure; < 0 never */

int od_hal_nvs_init(void) { return OD_HAL_NVS_OK; }
int od_hal_nvs_size(uint32_t *len)
{
    if (!g_rec_present) { return OD_HAL_NVS_ENOENT; }
    *len = g_rec_len;
    return OD_HAL_NVS_OK;
}
int od_hal_nvs_read(uint32_t off, void *buf, uint32_t len)
{
    if (!g_rec_present) { return OD_HAL_NVS_ENOENT; }
    if (off + len > g_rec_len) { return OD_HAL_NVS_E2BIG; }
    memcpy(buf, g_rec + off, len);
    return OD_HAL_NVS_OK;
}
int od_hal_nvs_write(const void *rec, uint32_t len)
{
    if (g_nvs_fail == 0) { return OD_HAL_NVS_EIO; }
    if (g_nvs_fail > 0) { --g_nvs_fail; }
    memcpy(g_rec, rec, len);
    g_rec_len = len;
    g_rec_present = true;
    return OD_HAL_NVS_OK;
}
int od_hal_nvs_erase(void)
{
    g_rec_present = false;
    g_rec_len = 0u;
    return OD_HAL_NVS_OK;
}

static void store_blob(const uint8_t *b, uint32_t n)
{
    static uint8_t ws[OD_CONFIG_STORE_MAX_RECORD];

    CHECK(od_config_store_save(ws, sizeof ws, b, n) == OD_CONFIG_STORE_OK);
}

static uint32_t stored(uint8_t *out)
{
    uint32_t n = OD_CONFIG_MAX_SIZE;

    if (od_config_store_load(out, &n) != OD_CONFIG_STORE_OK) {
        return 0u;
    }
    return n;
}

/* od_sec2's reply path is linked but never used here. */
od_radio_result_t od_hal_radio_send(od_origin_t o, uint32_t t, const uint8_t *f, uint16_t n)
{ (void)o; (void)t; (void)f; (void)n; return OD_RADIO_SENT; }
bool od_hal_radio_tag_is_live(od_origin_t o, uint32_t t) { (void)o; (void)t; return true; }
bool sec2c_radio_pop(uint8_t *buf, uint16_t *len) { (void)buf; (void)len; return false; }
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{ (void)rp; (void)len; (void)why; }

/* ------------------------------------------------------------------ od_sec2_app hooks --- */

static uint8_t g_uid[8];
static struct SecurityConfig g_parsed;    /* the target's parsed 0x27 */
static bool g_parsed_present;

uint32_t od_sec2_app_now_ms(void) { return 1000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, g_uid, 8u); }
uint8_t od_sec2_app_dflags(void) { return 0u; }
const struct SecurityConfig *od_sec2_app_security(void) { return g_parsed_present ? &g_parsed : NULL; }
bool od_sec2_app_display_busy(void) { return false; }

static bool find27(void *ctx, uint8_t id, od_span_t body)
{
    (void)ctx;
    if (id == 0x27u && body.n >= sizeof g_parsed) {
        memcpy(&g_parsed, body.p, sizeof g_parsed);
        g_parsed_present = true;
    }
    return true;
}

/* What a target does after a config change: re-parse 0x27 from the stored record. */
void od_claim_app_config_changed(void)
{
    uint8_t b[OD_CONFIG_MAX_SIZE];
    const uint32_t n = stored(b);

    g_parsed_present = false;
    if (n != 0u) {
        (void)od_config_tlv_walk(od_span_make(b, n), find27, NULL, NULL, NULL);
    }
}

static void put_key(uint8_t origin, uint16_t gen, const uint8_t key[16])
{
    struct od_sec2_key_rec k;
    struct od_sec2_state_rec s;
    uint8_t r[OD_SEC2_OS2K_LEN], t[OD_SEC2_OS2S_LEN];

    k.origin = origin;
    k.gen = gen;
    memcpy(k.key, key, 16u);
    od_sec2_os2k_encode(&k, r);
    CHECK(od_hal_secret_write(OD_HAL_SECRET_KEY, r, sizeof r));
    memset(&s, 0, sizeof s);
    s.key_gen = gen;
    s.flags = OD_SEC2_OS2S_MIGRATED;
    od_sec2_os2s_encode(&s, t);
    CHECK(od_hal_secret_write(OD_HAL_SECRET_STATE, t, sizeof t));
}

static void reset_all(void)
{
    fake_reset();
    g_rec_present = false;
    g_rec_len = 0u;
    g_nvs_fail = -1;
    g_parsed_present = false;
    memset(g_uid, 0, sizeof g_uid);
}

static void boot(void)
{
    od_claim_app_config_changed();
    (void)od_sec2_boot_ex(g_parsed_present ? &g_parsed : NULL, od_cfg_policy_stored_has_secrets());
}

/* [00 00 01] packets [crc] */
static uint32_t frame_blob(const uint8_t *packets, uint32_t n, uint8_t *out)
{
    uint16_t c;

    out[0] = 0u;
    out[1] = 0u;
    out[2] = 1u;
    memcpy(out + 3u, packets, n);
    c = od_config_tlv_crc16(od_span_make(out, 3u + n));
    out[3u + n] = (uint8_t)(c & 0xFFu);
    out[4u + n] = (uint8_t)(c >> 8);
    return n + 5u;
}

static uint8_t save(const uint8_t *b, uint32_t n)
{
    static uint8_t ws[OD_CONFIG_STORE_MAX_RECORD];

    if (od_cfg_policy_save(ws, sizeof ws, b, n) == OD_CONFIG_STORE_OK) {
        return OD_CFG_POLICY_OK;
    }
    return od_config_store_save_error();
}

/* ------------------------------------------------------------------------ the vectors --- */

static void test_v1_kdf(void)
{
    CASE("V1: KDF for every label and context length");
    for (size_t i = 0; i < N(V1); ++i) {
        uint8_t k[16], ctx[OD_SEC2_CTX_MAX], want[16], got[16];
        const size_t cn = unhex(V1[i].ctx, ctx, sizeof ctx);

        (void)unhex(V1[i].k, k, sizeof k);
        (void)unhex(V1[i].out, want, sizeof want);
        CHECK(od_sec2_kdf(k, V1[i].label, ctx, (uint16_t)cn, got) == OD_HAL_CRYPTO_OK);
        CHECK(memcmp(got, want, 16u) == 0);
    }
}

static void test_v10_secrets(void)
{
    for (size_t i = 0; i < N(V10); ++i) {
        uint8_t blob[OD_CONFIG_MAX_SIZE], pk[OD_CONFIG_MAX_SIZE], sb[OD_CONFIG_MAX_SIZE];
        const uint32_t n = (uint32_t)unhex(V10[i].blob, blob, sizeof blob);

        CASE(V10[i].name);
        reset_all();
        if (strcmp(V10[i].device, "claimed") == 0) {
            uint8_t k[16];
            (void)unhex(V10[i].k_dev, k, sizeof k);
            put_key(OD_SEC2_ORIGIN_CLAIMED, 1u, k);
        } else if (V10[i].stored != NULL) {
            const uint32_t pn = (uint32_t)unhex(V10[i].stored, pk, sizeof pk);
            store_blob(sb, frame_blob(pk, pn, sb));
        }
        boot();
        if (strstr(V10[i].device, "LEGACY") != NULL) {
            CHECK(od_sec2_legacy_secrets());
        }
        CHECK(save(blob, n) == (V10[i].verdict < 0 ? OD_CFG_POLICY_OK : (uint8_t)V10[i].verdict));
    }
}

static void read_view_matches(const uint8_t *rec, uint32_t n, const char *frame_hex)
{
    uint8_t frame[OD_CONFIG_MAX_SIZE + 6u], view[OD_CONFIG_MAX_SIZE];
    const uint32_t fn = (uint32_t)unhex(frame_hex, frame, sizeof frame);

    /* chunked as od_config_read does: 94 bytes in chunk 0, 96 after */
    for (uint32_t off = 0u; off < n;) {
        const uint32_t take = (n - off) < (off == 0u ? 94u : 96u) ? (n - off) : (off == 0u ? 94u : 96u);
        memcpy(view + off, rec + off, take);
        od_cfg_policy_read_view(rec, n, off, view + off, take);
        off += take;
    }
    /* The trailing CRC is excluded: SECURITY_V2 10.2 says no CRC is computed over the read
     * image, and the generator recomputes it for a claimed read. Recorded as a client-side
     * disagreement in the sec2-fw report; every other byte must match. */
    CHECK(fn == n + 6u);
    CHECK(fn == n + 6u && memcmp(view, frame + 6u, n - 2u) == 0);
    CHECK(memcmp(view + n - 2u, rec + n - 2u, 2u) == 0);
}

static void test_v11_at_rest(void)
{
    for (size_t i = 0; i < N(V11); ++i) {
        uint8_t before[OD_CONFIG_MAX_SIZE], w[OD_CONFIG_MAX_SIZE], after[OD_CONFIG_MAX_SIZE];
        uint8_t got[OD_CONFIG_MAX_SIZE];
        const uint32_t bn = (uint32_t)unhex(V11[i].before, before, sizeof before);
        uint32_t an = 0u, gn;

        CASE(V11[i].name);
        reset_all();
        store_blob(before, bn);
        if (V11[i].k_dev != NULL || V11[i].clear) {
            uint8_t k[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                             0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f};
            if (V11[i].k_dev != NULL) { (void)unhex(V11[i].k_dev, k, sizeof k); }
            if (V11[i].uid != NULL) { (void)unhex(V11[i].uid, g_uid, sizeof g_uid); }
            else { const uint8_t u[8] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77}; memcpy(g_uid, u, 8u); }
            put_key(OD_SEC2_ORIGIN_CLAIMED, 1u, k);
        }
        boot();
        if (V11[i].after != NULL) {
            an = (uint32_t)unhex(V11[i].after, after, sizeof after);
        }
        if (V11[i].clear) {
            CHECK(od_sec2_state() == OD_SEC2_CLAIMED);
            CHECK(od_cfg_policy_clear_store() == OD_CONFIG_STORE_OK);
            gn = stored(got);
            CHECK(gn == an && memcmp(got, after, an) == 0);
            continue;
        }
        if (V11[i].written != NULL) {
            const uint32_t wn = (uint32_t)unhex(V11[i].written, w, sizeof w);
            CHECK(od_sec2_state() == OD_SEC2_CLAIMED);
            CHECK(save(w, wn) == OD_CFG_POLICY_OK);
            gn = stored(got);
            CHECK(gn == an && memcmp(got, after, an) == 0);
        }
        gn = stored(got);
        if (V11[i].read != NULL) {
            read_view_matches(got, gn, V11[i].read);
        }
    }
}

static void test_v13_url(void)
{
    CASE("V13: setup URL");
    for (size_t i = 0; i < N(V13); ++i) {
        char url[128];

        CHECK(od_setup_url_build(V13[i].k, V13[i].i, (uint8_t)V13[i].t, (uint8_t)V13[i].ma,
                                 (uint8_t)V13[i].mi, (uint8_t)V13[i].pa, (uint16_t)V13[i].b,
                                 (uint16_t)V13[i].d, (uint16_t)V13[i].p, url, sizeof url));
        CHECK(strcmp(url, V13[i].url) == 0);
    }
}

/* ------------------------------------------------------- branches beyond the vectors --- */

static const uint8_t K1[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };

/* [00][26] WiFi with a password byte, [00][27] given body. */
static uint32_t pkt26(uint8_t *p, uint8_t pw)
{
    p[0] = 0u;
    p[1] = 0x26u;
    memset(p + 2u, 0, 160u);
    p[2] = 'N';
    p[2u + 32u] = pw;
    return 162u;
}

static uint32_t pkt27(uint8_t *p, uint8_t enabled, const uint8_t *key, uint8_t flags, uint8_t pin,
                      uint8_t res0)
{
    p[0] = 0u;
    p[1] = 0x27u;
    memset(p + 2u, 0, 64u);
    p[2] = enabled;
    if (key != NULL) { memcpy(p + 3u, key, 16u); }
    p[2u + 19u] = flags;
    p[2u + 20u] = pin;
    p[2u + 21u] = res0;
    return 66u;
}

static void test_branches(void)
{
    uint8_t pk[600], b[700], got[OD_CONFIG_MAX_SIZE];
    uint32_t n, gn;

    CASE("unclaimed: a planted SEC2 marker is stored as 0 (no remote path to LOCKED)");
    reset_all();
    boot();
    n = frame_blob(pk, pkt27(pk, 0u, NULL, 0u, 0xFFu, OD_SEC27_AT_REST_SEC2), b);
    CHECK(save(b, n) == OD_CFG_POLICY_OK);
    gn = stored(got);
    CHECK(gn == n && got[3u + 2u + 21u] == 0u);
    CHECK(od_config_tlv_crc16(od_span_make(got, gn - 2u)) ==
          (uint16_t)(got[gn - 2u] | (got[gn - 1u] << 8)));     /* a valid CRC stays valid */
    boot();
    CHECK(od_sec2_state() == OD_SEC2_UNCLAIMED);

    CASE("a dangling byte after the last packet: refused 0x00 on every device");
    reset_all();
    boot();
    n = pkt26(pk, 0u);
    pk[n++] = 0xAAu;
    n = frame_blob(pk, n, b);
    CHECK(save(b, n) == OD_ERR_CONFIG_WRITE_FAILED);
    CHECK(!g_rec_present);

    CASE("an unknown legacy id abandons the walk: refused 0x00");
    n = pkt26(pk, 0u);
    pk[n++] = 0u;
    pk[n++] = 0x3Fu;
    n = frame_blob(pk, n, b);
    CHECK(save(b, n) == OD_ERR_CONFIG_WRITE_FAILED);

    CASE("LOCKED: a secret is refused 0x10, a clean blob is stored, CONFIG_CLEAR is refused");
    reset_all();
    n = frame_blob(pk, pkt27(pk, 1u, K1, 0u, 0xFFu, OD_SEC27_AT_REST_SEC2), b);
    store_blob(b, n);
    boot();
    CHECK(od_sec2_state() == OD_SEC2_LOCKED);
    n = frame_blob(pk, pkt26(pk, 7u), b);
    CHECK(save(b, n) == OD_ERR_CONFIG_SECRET_NEEDS_CLAIM);
    CHECK(od_cfg_policy_clear_store() == OD_CONFIG_STORE_IO);
    CHECK(g_rec_present);

    CASE("claimed: a write disabling the reset pin keeps the stored pin (10.6)");
    reset_all();
    n = frame_blob(pk, pkt27(pk, 1u, K1, OD_SECURITY_FLAG_RESET_PIN_ENABLED |
                                         OD_SECURITY_FLAG_RESET_PIN_PULLUP, 7u,
                             OD_SEC27_AT_REST_SEC2), b);
    store_blob(b, n);
    put_key(OD_SEC2_ORIGIN_LEGACY, 1u, K1);
    boot();
    CHECK(od_sec2_state() == OD_SEC2_CLAIMED);
    n = frame_blob(pk, pkt27(pk, 0u, NULL, 0u, 0xFFu, 0u), b);
    CHECK(save(b, n) == OD_CFG_POLICY_OK);
    gn = stored(got);
    CHECK(got[3u + 2u + 0u] == 1u && memcmp(got + 6u, K1, 16u) == 0);   /* LEGACY: K_v1 = K_dev */
    CHECK(got[3u + 2u + 19u] == (OD_SECURITY_FLAG_RESET_PIN_ENABLED | OD_SECURITY_FLAG_RESET_PIN_PULLUP));
    CHECK(got[3u + 2u + 20u] == 7u && got[3u + 2u + 21u] == OD_SEC27_AT_REST_SEC2);

    CASE("claimed: a write that MOVES the pin while keeping it enabled is honoured");
    n = frame_blob(pk, pkt27(pk, 0u, NULL, OD_SECURITY_FLAG_RESET_PIN_ENABLED, 9u, 0u), b);
    CHECK(save(b, n) == OD_CFG_POLICY_OK);
    gn = stored(got);
    CHECK(got[3u + 2u + 19u] == OD_SECURITY_FLAG_RESET_PIN_ENABLED && got[3u + 2u + 20u] == 9u);

    CASE("claimed: no room for the 0x27 insertion is refused 0x00, nothing stored");
    {
        static uint8_t big[OD_CONFIG_MAX_SIZE];
        const uint32_t body = OD_CONFIG_MAX_SIZE - 9u;
        uint8_t keep[OD_CONFIG_MAX_SIZE];
        const uint32_t kn = stored(keep);

        memset(big, 0, sizeof big);
        big[2] = 1u;
        big[4] = 0xF0u;
        big[5] = (uint8_t)(body & 0xFFu);
        big[6] = (uint8_t)(body >> 8);
        CHECK(save(big, OD_CONFIG_MAX_SIZE) == OD_ERR_CONFIG_WRITE_FAILED);
        CHECK(stored(got) == kn && memcmp(got, keep, kn) == 0);
    }

    CASE("scrub: every secret zeroed, 0x27 unclaimed, the rest kept, one rewrite");
    reset_all();
    {
        uint32_t m = pkt26(pk, 0x55u);
        m += pkt26(pk + m, 0x66u);
        m += pkt27(pk + m, 1u, K1, OD_SECURITY_FLAG_RESET_PIN_ENABLED, 3u, OD_SEC27_AT_REST_SEC2);
        n = frame_blob(pk, m, b);
    }
    store_blob(b, n);
    put_key(OD_SEC2_ORIGIN_LEGACY, 1u, K1);
    boot();
    CHECK(od_cfg_policy_scrub());
    gn = stored(got);
    CHECK(gn == n);
    CHECK(got[3u + 2u + 32u] == 0u && got[3u + 162u + 2u + 32u] == 0u);
    CHECK(got[3u + 2u] == 'N');
    {
        const uint8_t *s27 = got + 3u + 324u + 2u;
        uint8_t zero[16] = {0};
        CHECK(s27[0] == 0u && memcmp(s27 + 1u, zero, 16u) == 0 && s27[21] == 0u);
        CHECK(s27[19] == OD_SECURITY_FLAG_RESET_PIN_ENABLED && s27[20] == 3u);
    }

    CASE("scrub: an unwalkable record is dropped whole");
    reset_all();
    n = pkt26(pk, 0x55u);
    pk[n++] = 0u;
    pk[n++] = 0x3Fu;              /* abandons the walk: what follows could hold anything */
    n = frame_blob(pk, n, b);
    store_blob(b, n);
    CHECK(od_cfg_policy_scrub());
    CHECK(!g_rec_present);

    CASE("scrub: a medium failure reports false");
    reset_all();
    n = frame_blob(pk, pkt26(pk, 0x55u), b);
    store_blob(b, n);
    g_nvs_fail = 0;
    CHECK(!od_cfg_policy_scrub());

    CASE("stale 2.x key (enabled 0, key set) is zeroed once, in place");
    reset_all();
    n = frame_blob(pk, pkt27(pk, 0u, K1, 0u, 0xFFu, 0u), b);
    store_blob(b, n);
    CHECK(od_cfg_policy_zero_stale_key());
    CHECK(!od_cfg_policy_zero_stale_key());
    gn = stored(got);
    {
        uint8_t zero[16] = {0};
        CHECK(memcmp(got + 6u, zero, 16u) == 0);
    }

    CASE("row 2 repair: a claimed device with no config record gets a 0x27-only one");
    reset_all();
    put_key(OD_SEC2_ORIGIN_LEGACY, 1u, K1);
    boot();
    od_cfg_policy_rest_sync();
    gn = stored(got);
    CHECK(gn == 71u && got[4] == 0x27u && got[5] == 1u && memcmp(got + 6u, K1, 16u) == 0);
    CHECK(got[5u + 21u] == OD_SEC27_AT_REST_SEC2);

    CASE("LEGACY_SECRETS: the identical secret is accepted, and one extra new one is not");
    reset_all();
    n = frame_blob(pk, pkt26(pk, 0x55u), b);
    store_blob(b, n);
    boot();
    CHECK(od_sec2_legacy_secrets());
    CHECK(save(b, n) == OD_CFG_POLICY_OK);
    {
        uint32_t m = pkt26(pk, 0x55u);
        m += pkt26(pk + m, 0x56u);
        n = frame_blob(pk, m, b);
    }
    CHECK(save(b, n) == OD_ERR_CONFIG_SECRET_NEEDS_CLAIM);
}

int main(void)
{
    test_v1_kdf();
    test_v10_secrets();
    test_v11_at_rest();
    test_v13_url();
    test_branches();
    printf("cfg_policy: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
