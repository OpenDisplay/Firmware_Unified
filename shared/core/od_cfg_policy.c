/* od_cfg_policy.c -- SECURITY_V2 sections 10.1-10.4 and 10.6 over the stored config record.
 * See od_cfg_policy.h for the rules; this file is their only implementation. */
#include "od_cfg_policy.h"

#include <string.h>

#include "od_claim_app.h"
#include "od_config_store.h"
#include "od_config_tlv.h"
#include "od_hal_crypto.h"
#include "od_log.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_sec2_wire.h"

/* 0x27 SecurityConfig body offsets (opendisplay_structs.h, 64 bytes). */
#define S27_ENABLED   0u
#define S27_KEY       1u
#define S27_FLAGS     19u
#define S27_PIN       20u
#define S27_RES0      21u
#define S27_LEN       64u
#define S27_PIN_BITS  (OD_SECURITY_FLAG_RESET_PIN_ENABLED | OD_SECURITY_FLAG_RESET_PIN_POLARITY | \
                       OD_SECURITY_FLAG_RESET_PIN_PULLUP | OD_SECURITY_FLAG_RESET_PIN_PULLDOWN)
/* 0x26 WifiConfig: password[32] at 32. 0x2D FindMyConfig: eik[32] at 8, net_a_adv_key[28] at 40,
 * advertisement_key[20] at 76. */
#define S26_PW        32u
#define S26_PW_LEN    32u
#define S2D_EIK       8u
#define S2D_NETA      40u
#define S2D_NETA_LEN  28u
#define S2D_ADV       76u
#define S2D_ADV_LEN   20u
#define S2D_MIN       96u

#define INSERT_LEN    (2u + S27_LEN)   /* [00][27][body] */

OD_STATIC_ASSERT(sizeof(struct SecurityConfig) == S27_LEN, "0x27 layout");
OD_STATIC_ASSERT(sizeof(struct WifiConfig) >= S26_PW + S26_PW_LEN, "0x26 layout");
OD_STATIC_ASSERT(sizeof(struct FindMyConfig) >= S2D_MIN, "0x2D layout");

/* The stored secrets of a LEGACY_SECRETS device, one 8-byte keyed digest per packet instance
 * that carries one, under a key drawn at boot. Keyed so a writer cannot construct different
 * secret bytes with the same digest; 8 bytes because RAM (BG22) and the bar is "adds no new
 * secret", not confidentiality. */
#define DIGESTS_MAX 4u
static struct {
    uint8_t key[16];
    uint8_t d[DIGESTS_MAX][8];
    uint8_t n;
    bool    valid;     /* key drawn and every stored instance fitted */
} g;

/* ------------------------------------------------------------------------- the scan --- */

struct scan {
    const uint8_t *base;
    uint32_t       last_end;        /* offset just past the last delivered body */
    uint32_t       insert_at;       /* header offset of the first packet with id > 0x27, or 0 */
    uint8_t       *p27[4];          /* every 0x27 instance (singletons are not trusted) */
    uint8_t        n27;
    bool           too_many_27;
    bool           secret;          /* any 0x26 / 0x2D secret byte */
    bool           key27;           /* any 0x27 with enabled != 0 or a non-zero key */
    bool           digest_miss;     /* a secret instance not among the stored digests */
    bool           record;          /* fill g.d (the boot scan) instead of checking against it */
    bool           scrub;           /* zero every secret field as it is found */
};

enum scan_mode { SCAN_CHECK = 0, SCAN_RECORD, SCAN_SCRUB };

static bool any_nonzero(const uint8_t *p, uint32_t n)
{
    uint8_t acc = 0u;
    uint32_t i;

    for (i = 0u; i < n; ++i) {
        acc = (uint8_t)(acc | p[i]);
    }
    return acc != 0u;
}

static bool digest(uint8_t id, const uint8_t *body, uint8_t out[8])
{
    uint8_t msg[1u + S2D_ADV + S2D_ADV_LEN];
    uint8_t mac[16];
    uint32_t n;
    bool ok;

    msg[0] = id;
    if (id == 0x26u) {
        memcpy(msg + 1u, body + S26_PW, S26_PW_LEN);
        n = 1u + S26_PW_LEN;
    } else {
        memcpy(msg + 1u, body + S2D_EIK, S2D_ADV + S2D_ADV_LEN - S2D_EIK);
        memset(msg + 1u + (S2D_NETA + S2D_NETA_LEN - S2D_EIK), 0, 8u);   /* the time bases */
        n = 1u + S2D_ADV + S2D_ADV_LEN - S2D_EIK;
    }
    ok = od_hal_crypto_cmac(g.key, msg, n, mac) == OD_HAL_CRYPTO_OK;
    memcpy(out, mac, 8u);
    memset(msg, 0, sizeof msg);
    memset(mac, 0, sizeof mac);
    return ok;
}

static void secret_instance(struct scan *sc, uint8_t id, const uint8_t *body)
{
    uint8_t d[8];
    uint8_t i;
    uint8_t diff;

    sc->secret = true;
    if (sc->scrub) {
        if (id == 0x26u) {
            memset((uint8_t *)(uintptr_t)body + S26_PW, 0, S26_PW_LEN);
        } else {
            memset((uint8_t *)(uintptr_t)body + S2D_EIK, 0, 32u);
            memset((uint8_t *)(uintptr_t)body + S2D_NETA, 0, S2D_NETA_LEN);
            memset((uint8_t *)(uintptr_t)body + S2D_ADV, 0, S2D_ADV_LEN);
        }
        return;
    }
    if (sc->record) {
        if (g.n >= DIGESTS_MAX || !digest(id, body, g.d[g.n])) {
            g.valid = false;
        } else {
            ++g.n;
        }
        return;
    }
    if (!g.valid || !digest(id, body, d)) {
        sc->digest_miss = true;
        return;
    }
    for (i = 0u; i < g.n; ++i) {
        uint8_t k;

        diff = 0u;
        for (k = 0u; k < 8u; ++k) {
            diff = (uint8_t)(diff | (d[k] ^ g.d[i][k]));
        }
        if (diff == 0u) {
            return;
        }
    }
    sc->digest_miss = true;
}

static bool scan_cb(void *ctx, uint8_t id, od_span_t body)
{
    struct scan *sc = (struct scan *)ctx;
    uint8_t *b = (uint8_t *)(uintptr_t)body.p;   /* the walk only reads; this module owns the blob */
    const uint32_t off = (uint32_t)(body.p - sc->base);
    const uint32_t hdr = id >= OD_CFG_TLV_LEN_PREFIXED_MIN ? 4u : 2u;

    sc->last_end = off + (uint32_t)body.n;
    if (id > 0x27u && sc->insert_at == 0u) {
        sc->insert_at = off - hdr;
    }
    switch (id) {
    case 0x26:
        if (body.n >= S26_PW + S26_PW_LEN && any_nonzero(b + S26_PW, S26_PW_LEN)) {
            secret_instance(sc, id, b);
        }
        break;
    case 0x2D:
        if (body.n >= S2D_MIN && (any_nonzero(b + S2D_EIK, 32u) ||
                                  any_nonzero(b + S2D_NETA, S2D_NETA_LEN) ||
                                  any_nonzero(b + S2D_ADV, S2D_ADV_LEN))) {
            secret_instance(sc, id, b);
        }
        break;
    case 0x27:
        if (body.n < S27_LEN) {
            break;
        }
        if (b[S27_ENABLED] != 0u || any_nonzero(b + S27_KEY, 16u)) {
            sc->key27 = true;
        }
        if (sc->n27 < (uint8_t)(sizeof sc->p27 / sizeof sc->p27[0])) {
            sc->p27[sc->n27++] = b;
        } else {
            sc->too_many_27 = true;
        }
        break;
    default:
        break;
    }
    return true;
}

/* One walk over blob[0..len). true = it ended exactly at the CRC. */
static bool scan_blob(uint8_t *blob, uint32_t len, struct scan *sc, enum scan_mode mode)
{
    uint8_t unknown = 0u;
    enum od_config_tlv_result r;

    memset(sc, 0, sizeof *sc);
    sc->base = blob;
    sc->record = mode == SCAN_RECORD;
    sc->scrub = mode == SCAN_SCRUB;
    sc->last_end = OD_CFG_TLV_HEADER_LEN;
    r = od_config_tlv_walk(od_span_make(blob, len), scan_cb, sc, NULL, &unknown);
    return r == OD_CFG_TLV_OK && unknown == 0u && len >= OD_CFG_TLV_HEADER_LEN + OD_CFG_TLV_CRC_LEN &&
           sc->last_end == len - OD_CFG_TLV_CRC_LEN && !sc->too_many_27;
}

static bool crc_ok(const uint8_t *blob, uint32_t len)
{
    const uint16_t stored = (uint16_t)((uint16_t)blob[len - 2u] | ((uint16_t)blob[len - 1u] << 8));

    return od_config_tlv_crc16(od_span_make(blob, len - 2u)) == stored;
}

static void crc_put(uint8_t *blob, uint32_t len)
{
    const uint16_t c = od_config_tlv_crc16(od_span_make(blob, len - 2u));

    blob[len - 2u] = (uint8_t)(c & 0xFFu);
    blob[len - 1u] = (uint8_t)(c >> 8);
}

/* ----------------------------------------------------------------- the 0x27 forms --- */

/* body becomes the at-rest form for the current key (section 10.2). `prev` is the 0x27 in force
 * (the reset pin is sticky, 10.6), or NULL. false = no key (not claimed). */
static bool at_rest(uint8_t body[S27_LEN], const struct SecurityConfig *prev)
{
    uint8_t kv1[16];

    if (!od_sec2_k_v1(kv1)) {
        return false;
    }
    if (prev != NULL && (prev->flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) != 0u &&
        (body[S27_FLAGS] & OD_SECURITY_FLAG_RESET_PIN_ENABLED) == 0u) {
        body[S27_FLAGS] = (uint8_t)((body[S27_FLAGS] & ~S27_PIN_BITS) | (prev->flags & S27_PIN_BITS));
        body[S27_PIN] = prev->reset_pin;
    }
    body[S27_ENABLED] = 1u;
    memcpy(body + S27_KEY, kv1, 16u);
    body[S27_RES0] = OD_SEC27_AT_REST_SEC2;
    memset(kv1, 0, sizeof kv1);
    return true;
}

/* The stored 0x27 at rest, for insertion or a CONFIG_CLEAR replacement. */
static bool stored_at_rest(uint8_t body[S27_LEN])
{
    const struct SecurityConfig *cur = od_sec2_app_security();

    if (cur != NULL) {
        memcpy(body, cur, S27_LEN);
    } else {
        memset(body, 0, S27_LEN);
        body[S27_PIN] = 0xFFu;
    }
    return at_rest(body, cur);
}

static void unclaimed_form(uint8_t *b)
{
    b[S27_ENABLED] = 0u;
    memset(b + S27_KEY, 0, 16u);
    b[S27_RES0] = 0u;
}

/* ------------------------------------------------------------------------- admit --- */

uint8_t od_cfg_policy_admit(uint8_t *blob, uint32_t *len, uint32_t cap)
{
    struct scan sc;
    enum od_sec2_state st;
    bool crc_was_ok;
    uint8_t i;

    if (blob == NULL || len == NULL || !scan_blob(blob, *len, &sc, SCAN_CHECK)) {
        return OD_ERR_CONFIG_WRITE_FAILED;
    }
    crc_was_ok = crc_ok(blob, *len);

#if !OD_SEC2
    (void)crc_was_ok;
    (void)st;
    (void)i;
    (void)cap;
    return (sc.secret || sc.key27) ? OD_ERR_CONFIG_SECRET_NEEDS_CLAIM : OD_CFG_POLICY_OK;
#else
    st = od_sec2_state();
    if (st == OD_SEC2_CLAIMED) {
        for (i = 0u; i < sc.n27; ++i) {
            if (any_nonzero(sc.p27[i] + S27_KEY, 16u) && !od_sec2_key_is(sc.p27[i] + S27_KEY)) {
                return OD_ERR_CONFIG_KEY_MISMATCH;
            }
        }
        for (i = 0u; i < sc.n27; ++i) {
            if (!at_rest(sc.p27[i], od_sec2_app_security())) {
                return OD_ERR_CONFIG_WRITE_FAILED;
            }
        }
        if (sc.n27 == 0u) {
            uint8_t body[S27_LEN];
            const uint32_t at = sc.insert_at != 0u ? sc.insert_at : *len - OD_CFG_TLV_CRC_LEN;

            if (*len + INSERT_LEN > cap || *len + INSERT_LEN > OD_CONFIG_MAX_SIZE ||
                !stored_at_rest(body)) {
                return OD_ERR_CONFIG_WRITE_FAILED;
            }
            memmove(blob + at + INSERT_LEN, blob + at, *len - at);
            blob[at] = 0u;
            blob[at + 1u] = 0x27u;
            memcpy(blob + at + 2u, body, S27_LEN);
            memset(body, 0, sizeof body);
            *len += INSERT_LEN;
        }
    } else if (st == OD_SEC2_UNCLAIMED) {
        if (sc.key27) {
            return OD_ERR_CONFIG_SECRET_NEEDS_CLAIM;
        }
        if (sc.secret && (!od_sec2_legacy_secrets() || sc.digest_miss)) {
            return OD_ERR_CONFIG_SECRET_NEEDS_CLAIM;
        }
        /* The marker is evidence of a claim at the next boot (10.1 row 3): an open writer must
         * not be able to plant it and lock the device. */
        for (i = 0u; i < sc.n27; ++i) {
            unclaimed_form(sc.p27[i]);
        }
    } else {
        if (sc.secret || sc.key27) {
            return OD_ERR_CONFIG_SECRET_NEEDS_CLAIM;
        }
    }
    if (crc_was_ok) {
        crc_put(blob, *len);
    }
    return OD_CFG_POLICY_OK;
#endif
}

enum od_config_store_result od_cfg_policy_save(void *workspace, uint32_t workspace_cap,
                                               const uint8_t *payload, uint32_t len)
{
    uint8_t *slot;
    uint32_t n = len;
    uint32_t cap;
    uint8_t rc;

    if (workspace == NULL || payload == NULL || workspace_cap < OD_CONFIG_STORE_HEADER_SIZE) {
        od_config_store_set_save_error(OD_ERR_CONFIG_WRITE_FAILED);
        return OD_CONFIG_STORE_IO;
    }
    cap = workspace_cap - OD_CONFIG_STORE_HEADER_SIZE;
    if (len > OD_CONFIG_MAX_SIZE || len > cap) {
        od_config_store_set_save_error(OD_ERR_CONFIG_WRITE_FAILED);
        return OD_CONFIG_STORE_TOO_BIG;
    }
    slot = od_config_store_payload(workspace);
    if (payload != slot) {
        memmove(slot, payload, len);
    }
    rc = od_cfg_policy_admit(slot, &n, cap);
    if (rc != OD_CFG_POLICY_OK) {
        od_log_warn("Config write refused by the SEC2 policy: 0x%02x", (unsigned)rc);
        od_config_store_set_save_error(rc);
        return OD_CONFIG_STORE_IO;
    }
    return od_config_store_save(workspace, workspace_cap, slot, n);
}

/* --------------------------------------------------------- stored-record rewrites --- */

/* The record loaded into the workspace: *blob / *len. false = none, busy or unreadable. */
static bool load_ws(uint8_t **ws_out, uint32_t *cap_out, uint8_t **blob, uint32_t *len)
{
    uint32_t cap = 0u;
    uint8_t *ws;

    if (od_claim_app_workspace_busy()) {
        return false;
    }
    ws = od_claim_app_workspace(&cap);
    if (ws == NULL || cap < OD_CONFIG_STORE_MAX_RECORD) {
        return false;
    }
    *blob = od_config_store_payload(ws);
    *len = cap - OD_CONFIG_STORE_HEADER_SIZE;
    *ws_out = ws;
    *cap_out = cap;
    return od_config_store_load(*blob, len) == OD_CONFIG_STORE_OK;
}

static bool store_only_27(uint8_t *ws, uint32_t cap)
{
    uint8_t *p = od_config_store_payload(ws);
    const uint32_t n = OD_CFG_TLV_HEADER_LEN + INSERT_LEN + OD_CFG_TLV_CRC_LEN;

    p[0] = 0u;
    p[1] = 0u;
    p[2] = 1u;
    p[3] = 0u;
    p[4] = 0x27u;
    if (!stored_at_rest(p + 5u)) {
        return false;
    }
    crc_put(p, n);
    return od_config_store_save(ws, cap, p, n) == OD_CONFIG_STORE_OK;
}

enum od_config_store_result od_cfg_policy_clear_store(void)
{
    uint32_t cap = 0u;
    uint8_t *ws;

#if OD_SEC2
    if (od_sec2_state() == OD_SEC2_CLAIMED) {
        if (od_claim_app_workspace_busy() || (ws = od_claim_app_workspace(&cap)) == NULL ||
            cap < OD_CONFIG_STORE_MAX_RECORD || !store_only_27(ws, cap)) {
            return OD_CONFIG_STORE_IO;
        }
        od_claim_app_config_changed();
        return OD_CONFIG_STORE_OK;
    }
    if (od_sec2_state() == OD_SEC2_LOCKED) {
        return OD_CONFIG_STORE_IO;    /* gated before here; never erase claim evidence */
    }
#endif
    (void)cap;
    (void)ws;
    return od_config_store_clear();
}

bool od_cfg_policy_stored_has_secrets(void)
{
    uint8_t *ws;
    uint8_t *blob;
    uint32_t cap;
    uint32_t len;
    struct scan sc;

    memset(&g, 0, sizeof g);
    if (!load_ws(&ws, &cap, &blob, &len)) {
        return false;
    }
    g.valid = od_hal_crypto_random(g.key, sizeof g.key) == OD_HAL_CRYPTO_OK;
    /* Scanned even when the walk is not clean: what it reached is what the loader stored. */
    (void)scan_blob(blob, len, &sc, SCAN_RECORD);
    return sc.secret;
}

bool od_cfg_policy_zero_stale_key(void)
{
    uint8_t *ws;
    uint8_t *blob;
    uint32_t cap;
    uint32_t len;
    struct scan sc;
    bool changed = false;
    bool crc_was_ok;
    uint8_t i;

    if (!load_ws(&ws, &cap, &blob, &len) || !scan_blob(blob, len, &sc, SCAN_CHECK)) {
        return false;
    }
    crc_was_ok = crc_ok(blob, len);
    for (i = 0u; i < sc.n27; ++i) {
        if (sc.p27[i][S27_ENABLED] == 0u && sc.p27[i][S27_RES0] != OD_SEC27_AT_REST_SEC2 &&
            any_nonzero(sc.p27[i] + S27_KEY, 16u)) {
            memset(sc.p27[i] + S27_KEY, 0, 16u);
            changed = true;
        }
    }
    if (!changed) {
        return false;
    }
    if (crc_was_ok) {
        crc_put(blob, len);
    }
    if (od_config_store_save(ws, cap, blob, len) != OD_CONFIG_STORE_OK) {
        return false;
    }
    od_claim_app_config_changed();
    return true;
}

/* ------------------------------------------------- scrub and 0x27 at rest (od_sec2) --- */

bool od_cfg_policy_scrub(void)
{
    uint8_t *ws;
    uint8_t *blob;
    uint32_t cap;
    uint32_t len;
    struct scan sc;
    bool crc_was_ok;
    uint8_t i;

    if (!load_ws(&ws, &cap, &blob, &len)) {
        uint8_t probe;
        uint32_t probe_len = 0u;

        /* No record: nothing to scrub there. Busy or unreadable: a failure. */
        if (od_claim_app_workspace_busy() ||
            od_config_store_load(&probe, &probe_len) != OD_CONFIG_STORE_EMPTY) {
            return false;
        }
        return od_claim_app_scrub_extras();
    }
    crc_was_ok = crc_ok(blob, len);
    if (!scan_blob(blob, len, &sc, SCAN_SCRUB)) {
        /* Not walkable to the end: what lies past the walk cannot be scrubbed field by field,
         * so the record goes whole (it may hold a secret the walk never reached). */
        return od_config_store_clear() == OD_CONFIG_STORE_OK && od_claim_app_scrub_extras();
    }
    for (i = 0u; i < sc.n27; ++i) {
        unclaimed_form(sc.p27[i]);
    }
    if (crc_was_ok) {
        crc_put(blob, len);
    }
    if (od_config_store_save(ws, cap, blob, len) != OD_CONFIG_STORE_OK) {
        return false;
    }
    od_claim_app_config_changed();
    return od_claim_app_scrub_extras();
}

void od_cfg_policy_rest_sync(void)
{
    uint8_t *ws;
    uint8_t *blob;
    uint32_t cap;
    uint32_t len;
    struct scan sc;
    uint8_t before[S27_LEN];
    bool changed = false;
    bool crc_was_ok;
    uint8_t i;

    if (od_sec2_state() != OD_SEC2_CLAIMED) {
        return;
    }
    if (!load_ws(&ws, &cap, &blob, &len)) {
        uint8_t probe;
        uint32_t probe_len = 0u;

        /* 10.1 row 2: the config record is gone, so write one holding only 0x27. */
        if (!od_claim_app_workspace_busy() &&
            od_config_store_load(&probe, &probe_len) == OD_CONFIG_STORE_EMPTY &&
            (ws = od_claim_app_workspace(&cap)) != NULL && cap >= OD_CONFIG_STORE_MAX_RECORD &&
            store_only_27(ws, cap)) {
            od_claim_app_config_changed();
        }
        return;
    }
    if (!scan_blob(blob, len, &sc, SCAN_CHECK)) {
        return;   /* a record this firmware cannot walk is not rewritten in place */
    }
    crc_was_ok = crc_ok(blob, len);
    for (i = 0u; i < sc.n27; ++i) {
        memcpy(before, sc.p27[i], S27_LEN);
        (void)at_rest(sc.p27[i], od_sec2_app_security());
        if (memcmp(before, sc.p27[i], S27_LEN) != 0) {
            changed = true;
        }
    }
    memset(before, 0, sizeof before);
    if (sc.n27 == 0u) {
        uint8_t body[S27_LEN];
        const uint32_t at = sc.insert_at != 0u ? sc.insert_at : len - OD_CFG_TLV_CRC_LEN;

        if (len + INSERT_LEN > OD_CONFIG_MAX_SIZE || !stored_at_rest(body)) {
            return;
        }
        memmove(blob + at + INSERT_LEN, blob + at, len - at);
        blob[at] = 0u;
        blob[at + 1u] = 0x27u;
        memcpy(blob + at + 2u, body, S27_LEN);
        memset(body, 0, sizeof body);
        len += INSERT_LEN;
        changed = true;
    }
    if (!changed) {
        return;
    }
    if (crc_was_ok) {
        crc_put(blob, len);
    }
    if (od_config_store_save(ws, cap, blob, len) == OD_CONFIG_STORE_OK) {
        od_claim_app_config_changed();
    }
}

/* ---------------------------------------------------------------------- read view --- */

struct view {
    const uint8_t *base;
    uint32_t off;
    uint8_t *dst;
    uint32_t n;
    bool claimed;
    uint8_t key[16];
};

static void patch(struct view *v, uint32_t at, uint8_t val)
{
    if (at >= v->off && at - v->off < v->n) {
        v->dst[at - v->off] = val;
    }
}

static bool view_cb(void *ctx, uint8_t id, od_span_t body)
{
    struct view *v = (struct view *)ctx;
    const uint32_t b = (uint32_t)(body.p - v->base);
    uint32_t k;

    if (id != 0x27u || body.n < S27_LEN) {
        return true;
    }
    if (b >= v->off + v->n || b + S27_LEN <= v->off) {
        return true;
    }
    patch(v, b + S27_ENABLED, v->claimed ? 1u : 0u);
    for (k = 0u; k < 16u; ++k) {
        patch(v, b + S27_KEY + k, v->key[k]);
    }
    patch(v, b + S27_RES0, 0u);
    return true;
}

void od_cfg_policy_read_view(const uint8_t *blob, uint32_t blob_len, uint32_t off,
                             uint8_t *dst, uint32_t n)
{
    struct view v;

    if (blob == NULL || dst == NULL || n == 0u) {
        return;
    }
    memset(&v, 0, sizeof v);
    v.base = blob;
    v.off = off;
    v.dst = dst;
    v.n = n;
    v.claimed = od_sec2_k_dev(v.key);
    (void)od_config_tlv_walk(od_span_make(blob, blob_len), view_cb, &v, NULL, NULL);
    memset(v.key, 0, sizeof v.key);
}
