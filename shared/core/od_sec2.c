/* od_sec2.c -- see od_sec2.h; docs/SECURITY_V2.md is the specification.
 *
 * RAM: one static struct. The pending union holds exactly one of a claim (Tc, K_new, PRK, code),
 * a handshake (Ta scratch, the expected client tag, the device tag, sid) or a rotation (K_new).
 * Record keys never sit here: a handshake installs them in HAL slots 0/1 at HELLO, and the
 * session only becomes live at FINISH.
 *
 * The 82-byte message buffer serves both directions: a request is reassembled into it, the reply
 * body is built over it, and the dispatcher defers the next SEC_CLAIM/SEC_AUTH frame until that
 * reply has left (od_sec2_tx_active), because BG22 has only two TX slots per dispatch and a
 * reply at ATT MTU 23 is up to six notifications.
 */
#include "od_sec2.h"

#include "od_hal_secret.h"
#include "od_nonce_window.h"
#include "od_reply.h"
#include "od_sec2_app.h"
#include "od_sec2_code.h"
#include "od_sec2_kdf.h"

#include <string.h>

#define SLOT_H2D 0u
#define SLOT_D2H 1u

OD_STATIC_ASSERT(OD_HAL_CRYPTO_KEY_SLOTS >= 2u, "SEC2 needs one CCM slot per direction");

/* Transcript layout (sections 8.2, 9.3): prefix || 0x02 || dflags || uid || Qc || Qd || Nc || Nd.
 * Offsets are relative to the end of the prefix: 9 bytes for Tc, 8 for Ta. */
#define T_VER    0u
#define T_DFLAGS 1u
#define T_UID    2u
#define T_QC     10u
#define T_QD     74u
#define T_NC     138u
#define T_ND     154u
#define T_TAIL   170u
#define TC_PFX   9u
#define TA_PFX   8u
OD_STATIC_ASSERT(TC_PFX + T_TAIL == OD_SEC2_TC_LEN, "Tc layout");
OD_STATIC_ASSERT(TA_PFX + T_TAIL == OD_SEC2_TA_LEN, "Ta layout");

enum pstep {
    P_NONE = 0,
    P_CLAIM_BEGUN,
    P_CLAIM_NONCED,
    P_AUTH_HELLOED,
    P_ROTATE
};

/* Fragment framing: [00][cmd_lo][sub][frag][chunk] (section 3.3). */
#define FRAG_HDR  4u
#define FRAG_LAST 0x80u
#define FRAG_IDX  0x7Fu
#define FRAG_MAX_MIN 20u
#define FRAG_MAX_MAX 244u

#define NO_REPLY 0xFFu   /* internal: the fragment was absorbed, nothing to send */

static struct {
    enum od_sec2_state state;
    uint8_t  origin;
    uint16_t gen;
    uint8_t  k_dev[16];
    struct od_sec2_state_rec os2s;
    bool     os2s_valid;

    /* the live session */
    bool       live;
    uint8_t    strikes;
    od_reply_t owner;
    uint32_t   tx_ctr;
    uint64_t   rx_last;
    uint64_t   rx_seen[OD_NONCE_BITMAP_WORDS];
    uint8_t    sid[16];
    uint32_t   start_ms;
    uint32_t   timeout_ms;

    /* the one pending claim / handshake / rotation */
    uint8_t    pstep;
    uint32_t   p_ms;
    od_reply_t p_rp;
    uint8_t    frag_max;
    union {
        struct {
            uint8_t  tc[OD_SEC2_TC_LEN];
            uint8_t  k_new[16];
            uint8_t  prk[16];
            uint32_t code;
        } c;
        struct {
            uint8_t ta[OD_SEC2_TC_LEN];   /* Ta is built here; 178 used */
            uint8_t exp_c[16];
            uint8_t tag_d[16];
            uint8_t sid[16];
        } a;
    } u;

    /* reassembly (rx) and fragment output (tx) over buf */
    bool       rx;
    uint16_t   rx_cmd;
    uint8_t    rx_sub;
    uint8_t    rx_next;
    uint8_t    rx_need;
    uint32_t   rx_ms;
    bool       tx;
    bool       tx_code;
    od_reply_t tx_rp;
    uint8_t    tx_cmd;
    uint8_t    tx_sub;
    uint8_t    tx_idx;
    uint8_t    tx_off;
    uint8_t    len;               /* bytes in buf: received so far, or reply body length */
    uint8_t    buf[OD_SEC2_BODY_MAX];

    /* handshake-start rate limiter (section 8.7): uptimes of the last 10 counted events */
    uint32_t ring[OD_SEC2_RATE_EVENTS];
    uint8_t  ring_n;
    uint8_t  ring_head;
} s;

/* ---------------------------------------------------------------------------- helpers --- */

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;

    while (n-- != 0u) {
        *v++ = 0u;
    }
}

static bool ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    volatile uint8_t d = 0u;
    size_t i;

    for (i = 0; i < n; ++i) {
        d = (uint8_t)(d | (a[i] ^ b[i]));
    }
    return d == 0u;
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0u;

    while (n-- != 0u) {
        acc |= *p++;
    }
    return acc == 0u;
}

static uint16_t timeout_s(void)
{
    const struct SecurityConfig *sec = od_sec2_app_security();
    const uint16_t t = (sec != NULL) ? sec->session_timeout_seconds : 0u;

    if (t == 0u) {
        return OD_SEC2_DEFAULT_TIMEOUT_S;
    }
    return (t < 60u) ? 60u : t;
}

static uint8_t base_dflags(void)
{
    return (uint8_t)(od_sec2_app_dflags() & (OD_SEC2_DFLAG_HAS_PANEL | OD_SEC2_DFLAG_HAS_RESET_PIN));
}

static bool write_os2s(void)
{
    uint8_t rec[OD_SEC2_OS2S_LEN];

    od_sec2_os2s_encode(&s.os2s, rec);
    s.os2s_valid = od_hal_secret_write(OD_HAL_SECRET_STATE, rec, sizeof rec);
    return s.os2s_valid;
}

static bool write_os2k(uint8_t origin, uint16_t gen, const uint8_t key[16])
{
    struct od_sec2_key_rec k;
    uint8_t rec[OD_SEC2_OS2K_LEN];
    bool ok;

    k.origin = origin;
    k.gen = gen;
    memcpy(k.key, key, 16u);
    od_sec2_os2k_encode(&k, rec);
    ok = od_hal_secret_write(OD_HAL_SECRET_KEY, rec, sizeof rec);
    wipe(&k, sizeof k);
    wipe(rec, sizeof rec);
    return ok;
}

static void set_key(uint8_t origin, uint16_t gen, const uint8_t key[16])
{
    s.state = OD_SEC2_CLAIMED;
    s.origin = origin;
    s.gen = gen;
    memcpy(s.k_dev, key, 16u);
}

static bool rate_allows(uint32_t now)
{
    return s.ring_n < OD_SEC2_RATE_EVENTS ||
           (uint32_t)(now - s.ring[s.ring_head]) >= OD_SEC2_RATE_WINDOW_MS;
}

static void rate_count(uint32_t now)
{
    s.ring[s.ring_head] = now;
    s.ring_head = (uint8_t)((s.ring_head + 1u) % OD_SEC2_RATE_EVENTS);
    if (s.ring_n < OD_SEC2_RATE_EVENTS) {
        s.ring_n++;
    }
}

/* ------------------------------------------------------------------------- state, boot --- */

enum od_sec2_state od_sec2_boot(const struct SecurityConfig *sec)
{
    uint8_t rec[OD_SEC2_OS2K_LEN];
    struct od_sec2_key_rec k;
    bool key_valid;

    od_hal_crypto_ecdh_discard();
    od_hal_crypto_key_clear(SLOT_H2D);
    od_hal_crypto_key_clear(SLOT_D2H);
    wipe(&s, sizeof s);

    key_valid = od_hal_secret_read(OD_HAL_SECRET_KEY, rec, OD_SEC2_OS2K_LEN) &&
                od_sec2_os2k_decode(rec, &k);
    s.os2s_valid = od_hal_secret_read(OD_HAL_SECRET_STATE, rec, OD_SEC2_OS2S_LEN) &&
                   od_sec2_os2s_decode(rec, &s.os2s);
    if (!s.os2s_valid) {
        memset(&s.os2s, 0, sizeof s.os2s);
    }

    if (key_valid) {
        if (s.os2s_valid && k.gen < s.os2s.key_gen) {
            s.state = OD_SEC2_LOCKED;                       /* row 1: an older key came back */
        } else {
            set_key(k.origin, k.gen, k.key);                /* row 2 */
            if (!s.os2s_valid || s.os2s.key_gen != k.gen) {
                s.os2s.key_gen = k.gen;
                s.os2s.flags |= OD_SEC2_OS2S_MIGRATED;
                (void)write_os2s();
            }
        }
    } else if (sec != NULL && sec->reserved[0] == OD_SEC27_AT_REST_SEC2) {
        s.state = OD_SEC2_LOCKED;                           /* row 3: claim evidence, no key */
    } else if (sec != NULL && sec->encryption_enabled != 0u &&
               !all_zero(sec->encryption_key, 16u)) {
        /* Row 4, migration B2. OS2S first: a cut between the writes then leaves key_gen 1 and
         * no OS2K, which re-runs this row, instead of an OS2K gen 1 under an older, higher
         * key_gen, which row 1 would lock. The key is in force for this boot either way. */
        s.os2s.key_gen = 1u;
        s.os2s.flags |= OD_SEC2_OS2S_MIGRATED;
        (void)write_os2s();
        (void)write_os2k(OD_SEC2_ORIGIN_LEGACY, 1u, sec->encryption_key);
        set_key(OD_SEC2_ORIGIN_LEGACY, 1u, sec->encryption_key);
    } else {
        s.state = OD_SEC2_UNCLAIMED;                        /* row 8 (rows 5-7: WP4) */
    }
    wipe(rec, sizeof rec);
    wipe(&k, sizeof k);
    return s.state;
}

enum od_sec2_state od_sec2_state(void)
{
    return s.state;
}

bool od_sec2_needs_session(void)
{
    return s.state != OD_SEC2_UNCLAIMED;
}

static bool legacy_secrets_open(void)
{
    return s.state == OD_SEC2_UNCLAIMED && s.os2s_valid &&
           (s.os2s.flags & OD_SEC2_OS2S_LEGACY_SECRETS) != 0u;
}

uint8_t od_sec2_ident_flags(void)
{
    struct od_sec2_claim_mode cm;
    uint8_t f = OD_IDENT_FLAG_SEC_V2;

    od_sec2_app_claim_mode(&cm);
    if (s.state != OD_SEC2_UNCLAIMED) {
        f |= OD_IDENT_FLAG_CLAIMED;
    }
    if (cm.active) {
        f |= OD_IDENT_FLAG_CLAIM_MODE;
    }
    if (legacy_secrets_open()) {
        f |= OD_IDENT_FLAG_LEGACY_SECRETS;
    }
    return f;
}

static bool derive_from_kdev(const char *label, uint8_t out[16])
{
    uint8_t uid[OD_SEC2_UID_LEN];

    if (s.state != OD_SEC2_CLAIMED) {
        return false;
    }
    od_sec2_app_uid(uid);
    return od_sec2_kdf(s.k_dev, label, uid, sizeof uid, out) == OD_HAL_CRYPTO_OK;
}

bool od_sec2_k_lan(uint8_t out[16])
{
    return derive_from_kdev("OD2/k-lan", out);
}

bool od_sec2_k_v1(uint8_t out[16])
{
    if (s.state == OD_SEC2_CLAIMED && s.origin == OD_SEC2_ORIGIN_LEGACY) {
        memcpy(out, s.k_dev, 16u);       /* a migrated, never rotated key IS the v1 key */
        return true;
    }
    return derive_from_kdev("OD2/v1-compat", out);
}

/* ---------------------------------------------------------------------------- session --- */

void od_sec2_teardown(void)
{
    if (s.live) {
        od_hal_crypto_key_clear(SLOT_H2D);
        od_hal_crypto_key_clear(SLOT_D2H);
    }
    s.live = false;
    s.strikes = 0u;
    s.tx_ctr = 0u;
    s.rx_last = 0u;
    memset(s.rx_seen, 0, sizeof s.rx_seen);
    wipe(s.sid, sizeof s.sid);
    if (s.pstep == P_ROTATE) {
        s.pstep = P_NONE;                 /* the pending key dies with the session */
        wipe(&s.u, sizeof s.u);
    }
}

bool od_sec2_session_live(const od_reply_t *rp)
{
    if (!s.live) {
        return false;
    }
    if ((uint32_t)(od_sec2_app_now_ms() - s.start_ms) >= s.timeout_ms) {
        od_sec2_teardown();
        return false;
    }
    return rp == NULL || od_reply_same(rp, &s.owner);
}

static void session_open(const od_reply_t *rp, const uint8_t sid[16], uint32_t now)
{
    s.live = true;
    s.strikes = 0u;
    s.tx_ctr = 0u;
    s.rx_last = 0u;
    memset(s.rx_seen, 0, sizeof s.rx_seen);
    memcpy(s.sid, sid, 16u);
    s.owner = *rp;
    s.start_ms = now;
    s.timeout_ms = (uint32_t)timeout_s() * 1000u;
}

/* k_h2d, k_d2h into the slots, sid out: KDF(PRK, "OD2/h2d" | "OD2/d2h" | "OD2/sid", T). */
static enum od_hal_crypto_status derive_session(const uint8_t prk[16], const uint8_t *t,
                                                uint16_t tlen, uint8_t sid[16])
{
    uint8_t k[16];
    enum od_hal_crypto_status cs;

    cs = od_sec2_kdf(prk, "OD2/h2d", t, tlen, k);
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_hal_crypto_key_set(SLOT_H2D, k);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_kdf(prk, "OD2/d2h", t, tlen, k);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_hal_crypto_key_set(SLOT_D2H, k);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_kdf(prk, "OD2/sid", t, tlen, sid);
    }
    wipe(k, sizeof k);
    return cs;
}

/* End the pending claim / handshake / rotation. `failed` reports a claim that did not succeed
 * to claim mode, which counts a rendered one against the session's attempts. */
static void clear_pending(bool failed)
{
    if (failed && (s.pstep == P_CLAIM_BEGUN || s.pstep == P_CLAIM_NONCED)) {
        od_sec2_app_claim_event(OD_SEC2_EV_DISCARDED, s.pstep == P_CLAIM_NONCED ? 1u : 0u);
    }
    if (failed && s.pstep == P_AUTH_HELLOED && !s.live) {
        od_hal_crypto_key_clear(SLOT_H2D);
        od_hal_crypto_key_clear(SLOT_D2H);
    }
    od_hal_crypto_ecdh_discard();
    wipe(&s.u, sizeof s.u);
    s.pstep = P_NONE;
}

void od_sec2_disconnect(void)
{
    od_sec2_teardown();
    clear_pending(true);
    s.rx = false;
    s.tx = false;
    s.len = 0u;
}

/* ---------------------------------------------------------------------- record layer --- */

static void nonce13(uint8_t n[13], uint8_t dir, uint32_t ctr)
{
    memset(n, 0, 13u);
    n[0] = dir;
    wr32(&n[1], ctr);
}

enum od_sec2_open od_sec2_open(uint16_t cmd, od_span_t rec, uint8_t *out, size_t out_cap,
                               uint16_t *out_len)
{
    uint8_t nonce[13];
    uint8_t aad[3];
    uint32_t ctr;
    uint16_t plain_len = 0u;
    enum od_hal_crypto_status cs;

    if (!s.live) {
        return OD_SEC2_OPEN_NO_SESSION;
    }
    if (rec.n < OD_SEC2_H2D_MIN || rec.p == NULL) {
        return OD_SEC2_OPEN_RECORD;
    }
    ctr = rd32(rec.p);
    if (ctr == 0u || ctr == 0xFFFFFFFFu ||
        od_nonce_check(s.rx_seen, s.rx_last, ctr) != NONCE_OK) {
        return OD_SEC2_OPEN_REPLAY;
    }
    if (out == NULL || out_len == NULL || out_cap < rec.n - OD_SEC2_H2D_MIN) {
        return OD_SEC2_OPEN_NO_ROOM;
    }
    nonce13(nonce, 0x48u, ctr);
    aad[0] = OD_SEC2_VERSION;
    aad[1] = (uint8_t)(cmd >> 8);
    aad[2] = (uint8_t)cmd;
    cs = od_hal_crypto_ccm_decrypt(SLOT_H2D, nonce, 13u, aad, 3u,
                                   rec.p + OD_SEC2_REC_HDR,
                                   (uint16_t)(rec.n - OD_SEC2_REC_HDR),
                                   out, (uint16_t)out_cap, &plain_len);
    if (cs == OD_HAL_CRYPTO_AUTH_FAILED) {
        if (++s.strikes >= OD_SEC2_STRIKES) {
            return OD_SEC2_OPEN_BAD_TAG_TEARDOWN;
        }
        return OD_SEC2_OPEN_BAD_TAG;
    }
    if (cs != OD_HAL_CRYPTO_OK) {
        return OD_SEC2_OPEN_CRYPTO_ERROR;
    }
    od_nonce_commit(s.rx_seen, &s.rx_last, ctr);
    *out_len = plain_len;
    return OD_SEC2_OPEN_OK;
}

enum od_sec2_seal od_sec2_seal(od_span_t frame, uint8_t *out, size_t out_cap, uint16_t *out_len)
{
    static const uint8_t aad[2] = { OD_SEC2_VERSION, OD_SEC2_SEALED_STATUS };
    uint8_t nonce[13];
    uint16_t ct_len = 0u;
    uint32_t ctr;

    if (!s.live) {
        return OD_SEC2_SEAL_NO_SESSION;
    }
    if (frame.n > OD_SEC2_PLAIN_RESP_MAX) {
        return OD_SEC2_SEAL_TOO_LONG;
    }
    if (out == NULL || out_len == NULL || out_cap < frame.n + OD_SEC2_D2H_OVERHEAD) {
        return OD_SEC2_SEAL_NO_ROOM;
    }
    if (s.tx_ctr >= OD_SEC2_CTR_MAX) {
        od_sec2_teardown();
        return OD_SEC2_SEAL_EXHAUSTED;
    }
    ctr = ++s.tx_ctr;
    out[0] = OD_SEC2_SEALED_STATUS;
    wr32(&out[1], ctr);
    nonce13(nonce, 0x44u, ctr);
    if (od_hal_crypto_ccm_encrypt(SLOT_D2H, nonce, 13u, aad, 2u, frame.p, (uint16_t)frame.n,
                                  &out[1u + OD_SEC2_REC_HDR],
                                  (uint16_t)(out_cap - 1u - OD_SEC2_REC_HDR),
                                  &ct_len) != OD_HAL_CRYPTO_OK) {
        return OD_SEC2_SEAL_CRYPTO_ERROR;
    }
    *out_len = (uint16_t)(1u + OD_SEC2_REC_HDR + ct_len);
    if (ctr == OD_SEC2_CTR_MAX) {
        od_sec2_teardown();               /* nothing more can be sealed under this key */
    }
    return OD_SEC2_SEAL_OK;
}

/* -------------------------------------------------------------------- fragment output --- */

static od_txq_status_t emit_fragment(od_tx_reservation_t *r)
{
    uint8_t f[FRAG_HDR + FRAG_MAX_MAX];
    const uint8_t room = (uint8_t)(s.frag_max - FRAG_HDR);
    const uint8_t left = (uint8_t)(s.len - s.tx_off);
    const uint8_t take = (left < room) ? left : room;
    const bool last = take == left;
    od_txq_status_t rc;

    f[0] = RESP_ACK;
    f[1] = s.tx_cmd;
    f[2] = s.tx_sub;
    f[3] = (uint8_t)(s.tx_idx | (last ? FRAG_LAST : 0u));
    memcpy(&f[FRAG_HDR], &s.buf[s.tx_off], take);
    rc = od_reply_plain(r, &s.tx_rp, f, (uint16_t)(FRAG_HDR + take));
    if (rc != OD_TXQ_OK) {
        return rc;
    }
    s.tx_off = (uint8_t)(s.tx_off + take);
    s.tx_idx++;
    if (last) {
        s.tx = false;
        s.len = 0u;
        if (s.tx_code) {
            s.tx_code = false;
            od_sec2_app_claim_event(OD_SEC2_EV_CODE, s.u.c.code);
        }
    }
    return OD_TXQ_OK;
}

void od_sec2_pump(void)
{
    od_tx_reservation_t r;
    od_txq_status_t rc;

    if (!s.tx || od_txq_reserve(1u, &r) != OD_TXQ_OK) {
        return;
    }
    rc = emit_fragment(&r);
    od_txq_release(&r);
    if (rc != OD_TXQ_OK && rc != OD_TXQ_FULL) {
        s.tx = false;                     /* the peer is gone: the reply cannot complete */
        s.tx_code = false;
    }
}

bool od_sec2_tx_active(void)
{
    return s.tx;
}

static void reply_error(od_tx_reservation_t *r, const od_reply_t *rp, uint8_t cmd_lo,
                        uint8_t sub, uint8_t err)
{
    uint8_t f[4];

    f[0] = RESP_NACK;
    f[1] = cmd_lo;
    f[2] = sub;
    f[3] = err;
    (void)od_reply_plain(r, rp, f, sizeof f);
}

/* ------------------------------------------------------------------------------- claim --- */

static uint8_t body_len(uint16_t cmd, uint8_t sub)
{
    static const uint8_t claim[5] = { 66u, 16u, 16u, 0u, 0u };
    static const uint8_t auth[2] = { 82u, 16u };

    if (cmd == CMD_SEC_CLAIM && sub >= 1u && sub <= 5u) {
        return claim[sub - 1u];
    }
    if (cmd == CMD_SEC_AUTH && sub >= 1u && sub <= 2u) {
        return auth[sub - 1u];
    }
    return NO_REPLY;
}

/* A claim ended without success. When it was the third render, the event that ended it is
 * answered ATTEMPTS_EXHAUSTED and claim mode ends (section 7.5). */
static uint8_t fail_claim(uint8_t err)
{
    const bool rendered = s.pstep == P_CLAIM_NONCED;
    struct od_sec2_claim_mode cm;

    clear_pending(true);
    od_sec2_app_claim_mode(&cm);
    if (rendered && cm.active && cm.renders_left == 0u) {
        od_sec2_app_claim_event(OD_SEC2_EV_EXHAUSTED, 0u);
        return OD_SEC_ERR_ATTEMPTS_EXHAUSTED;
    }
    return err;
}

static uint8_t claim_begin(const od_reply_t *rp, uint32_t now)
{
    uint8_t *const tc = s.u.c.tc;
    uint8_t *const t = tc + TC_PFX;
    struct od_sec2_claim_mode cm;
    uint8_t frag_max = s.buf[1];
    uint8_t qc[OD_SEC2_POINT_LEN];
    uint8_t err = 0u;

    if (s.buf[0] != OD_SEC2_VERSION || frag_max < FRAG_MAX_MIN || frag_max > FRAG_MAX_MAX) {
        return fail_claim(OD_SEC_ERR_MALFORMED);
    }
    if (!rate_allows(now)) {
        return OD_SEC_ERR_RATE_LIMITED;
    }
    rate_count(now);
    memcpy(qc, &s.buf[2], sizeof qc);
    err = fail_claim(0u);                 /* a new BEGIN discards the pending claim */
    if (err != 0u) {
        return err;
    }
    if (!od_sec2_point_valid(qc)) {
        return OD_SEC_ERR_BAD_POINT;
    }
    od_sec2_app_claim_mode(&cm);
    memcpy(tc, "OD2/claim", TC_PFX);
    t[T_VER] = OD_SEC2_VERSION;
    t[T_DFLAGS] = (uint8_t)(base_dflags() | (cm.fresh ? OD_SEC2_DFLAG_CLAIM_MODE_FRESH : 0u));
    od_sec2_app_uid(&t[T_UID]);
    memcpy(&t[T_QC], qc, sizeof qc);
    if (od_hal_crypto_ecdh_keygen(&t[T_QD]) != OD_HAL_CRYPTO_OK ||
        od_hal_crypto_random(&t[T_ND], 16u) != OD_HAL_CRYPTO_OK ||
        od_hal_crypto_random(s.u.c.k_new, 16u) != OD_HAL_CRYPTO_OK ||
        od_sec2_commit(&t[T_ND], &t[T_UID], &t[T_QD], &t[T_QC], &s.buf[65]) != OD_HAL_CRYPTO_OK) {
        clear_pending(false);
        return OD_SEC_ERR_CRYPTO_ERROR;
    }
    s.buf[0] = t[T_DFLAGS];
    memcpy(&s.buf[1], &t[T_QD], OD_SEC2_POINT_LEN);
    s.len = 81u;
    s.frag_max = frag_max;
    s.pstep = P_CLAIM_BEGUN;
    s.p_ms = now;
    s.p_rp = *rp;
    return 0u;
}

static uint8_t claim_nonce(uint32_t now)
{
    uint8_t *const tc = s.u.c.tc;
    uint8_t *const t = tc + TC_PFX;
    uint8_t z[32];
    uint8_t k[16];
    uint8_t aad[16];
    uint16_t wlen = 0u;
    static const uint8_t zero_nonce[13];
    enum od_hal_crypto_status cs;

    if ((uint32_t)(now - s.p_ms) > OD_SEC2_BEGIN_TO_NONCE_MS) {
        return fail_claim(OD_SEC_ERR_EXPIRED);
    }
    memcpy(&t[T_NC], s.buf, 16u);
    cs = od_hal_crypto_ecdh_agree(&t[T_QC], z);
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_extract(NULL, z, s.u.c.prk);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_kdf(s.u.c.prk, "OD2/wrap", tc, OD_SEC2_TC_LEN, k);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        /* k_wrap is single-use, so the constant nonce is safe (section 19 E10). The device is
         * unclaimed, so slot 0 holds no session key. */
        cs = od_hal_crypto_key_set(SLOT_H2D, k);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        memcpy(aad, "OD2/wrap", 8u);
        memcpy(&aad[8], &t[T_UID], 8u);
        cs = od_hal_crypto_ccm_encrypt(SLOT_H2D, zero_nonce, 13u, aad, 16u, s.u.c.k_new, 16u,
                                       &s.buf[16], 28u, &wlen);
        od_hal_crypto_key_clear(SLOT_H2D);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_code(&t[T_NC], &t[T_UID], &t[T_QC], &t[T_QD], &t[T_ND], &s.u.c.code);
    }
    wipe(z, sizeof z);
    wipe(k, sizeof k);
    if (cs != OD_HAL_CRYPTO_OK) {
        return fail_claim(cs == OD_HAL_CRYPTO_BAD_POINT ? OD_SEC_ERR_BAD_POINT
                                                        : OD_SEC_ERR_CRYPTO_ERROR);
    }
    memcpy(s.buf, &t[T_ND], 16u);
    s.len = 44u;
    s.pstep = P_CLAIM_NONCED;
    s.p_ms = now;
    s.tx_code = true;
    return 0u;
}

static uint8_t claim_confirm(const od_reply_t *rp, uint32_t now)
{
    uint8_t *const tc = s.u.c.tc;
    uint8_t kconf[16];
    uint8_t tag[16];
    uint8_t sid[16];
    uint16_t gen;
    uint16_t to;
    uint8_t err = 0u;

    if ((uint32_t)(now - s.p_ms) > OD_SEC2_NONCE_TO_CONFIRM_MS) {
        return fail_claim(OD_SEC_ERR_EXPIRED);
    }
    if (od_sec2_kdf(s.u.c.k_new, "OD2/k-conf", &tc[TC_PFX + T_UID], 8u, kconf) !=
            OD_HAL_CRYPTO_OK ||
        od_sec2_kdf(kconf, "OD2/claim-c", tc, OD_SEC2_TC_LEN, tag) != OD_HAL_CRYPTO_OK) {
        err = fail_claim(OD_SEC_ERR_CRYPTO_ERROR);
        goto cleanup;
    }
    if (!ct_equal(tag, s.buf, 16u)) {
        rate_count(now);
        err = fail_claim(OD_SEC_ERR_AUTH_FAILED);
        goto cleanup;
    }
    gen = (uint16_t)(s.os2s.key_gen + 1u);
    if (!write_os2k(OD_SEC2_ORIGIN_CLAIMED, gen, s.u.c.k_new)) {
        err = fail_claim(OD_SEC_ERR_STORAGE_ERROR);
        goto cleanup;
    }
    /* The claim stands from here on. Failures below are repaired at the next boot (10.1). */
    s.os2s.key_gen = gen;
    s.os2s.flags |= OD_SEC2_OS2S_MIGRATED;
    (void)write_os2s();
    set_key(OD_SEC2_ORIGIN_CLAIMED, gen, s.u.c.k_new);
    od_sec2_app_rest_sync();
    od_sec2_app_claim_event(OD_SEC2_EV_CLAIMED, 0u);

    if (od_sec2_kdf(kconf, "OD2/claim-d", tc, OD_SEC2_TC_LEN, s.buf) != OD_HAL_CRYPTO_OK ||
        derive_session(s.u.c.prk, tc, OD_SEC2_TC_LEN, sid) != OD_HAL_CRYPTO_OK) {
        od_hal_crypto_key_clear(SLOT_H2D);
        od_hal_crypto_key_clear(SLOT_D2H);
        clear_pending(false);
        err = OD_SEC_ERR_CRYPTO_ERROR;    /* claimed; the client authenticates with SEC_AUTH */
        goto cleanup;
    }
    session_open(rp, sid, now);
    to = timeout_s();
    s.buf[16] = (uint8_t)to;
    s.buf[17] = (uint8_t)(to >> 8);
    s.len = 18u;
    clear_pending(false);
cleanup:
    wipe(kconf, sizeof kconf);
    wipe(tag, sizeof tag);
    wipe(sid, sizeof sid);
    return err;
}

/* ------------------------------------------------------------------------------- auth --- */

static uint8_t auth_hello(const od_reply_t *rp, uint32_t now)
{
    uint8_t *const ta = s.u.a.ta;
    uint8_t *const t = ta + TA_PFX;
    uint8_t frag_max = s.buf[1];
    uint8_t z[32];
    uint8_t k[16];
    enum od_hal_crypto_status cs;

    if (s.buf[0] != OD_SEC2_VERSION || frag_max < FRAG_MAX_MIN || frag_max > FRAG_MAX_MAX) {
        clear_pending(true);
        return OD_SEC_ERR_MALFORMED;
    }
    if (!rate_allows(now)) {
        return OD_SEC_ERR_RATE_LIMITED;
    }
    rate_count(now);
    od_sec2_teardown();                   /* a HELLO replaces the live session */
    clear_pending(true);
    if (!od_sec2_point_valid(&s.buf[2])) {
        return OD_SEC_ERR_BAD_POINT;
    }
    memcpy(ta, "OD2/auth", TA_PFX);
    t[T_VER] = OD_SEC2_VERSION;
    t[T_DFLAGS] = (uint8_t)(base_dflags() |
                            (s.origin == OD_SEC2_ORIGIN_LEGACY ? OD_SEC2_DFLAG_KEY_LEGACY : 0u));
    od_sec2_app_uid(&t[T_UID]);
    memcpy(&t[T_QC], &s.buf[2], OD_SEC2_POINT_LEN);
    memcpy(&t[T_NC], &s.buf[66], 16u);

    cs = od_hal_crypto_ecdh_keygen(&t[T_QD]);
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_hal_crypto_random(&t[T_ND], 16u);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_hal_crypto_ecdh_agree(&t[T_QC], z);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_kdf(s.k_dev, "OD2/k-ext", &t[T_UID], 8u, k);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_extract(k, z, k);    /* k becomes PRK_auth */
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_kdf(k, "OD2/auth-c", ta, OD_SEC2_TA_LEN, s.u.a.exp_c);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = od_sec2_kdf(k, "OD2/auth-d", ta, OD_SEC2_TA_LEN, s.u.a.tag_d);
    }
    if (cs == OD_HAL_CRYPTO_OK) {
        cs = derive_session(k, ta, OD_SEC2_TA_LEN, s.u.a.sid);
    }
    wipe(z, sizeof z);
    wipe(k, sizeof k);
    s.pstep = P_AUTH_HELLOED;             /* so a failure below releases the slots */
    if (cs != OD_HAL_CRYPTO_OK) {
        clear_pending(true);
        return cs == OD_HAL_CRYPTO_BAD_POINT ? OD_SEC_ERR_BAD_POINT : OD_SEC_ERR_CRYPTO_ERROR;
    }
    s.buf[0] = t[T_DFLAGS];
    memcpy(&s.buf[1], &t[T_QD], OD_SEC2_POINT_LEN);
    memcpy(&s.buf[65], &t[T_ND], 16u);
    wipe(ta, OD_SEC2_TC_LEN);             /* keep only the tags and sid */
    s.len = 81u;
    s.frag_max = frag_max;
    s.p_ms = now;
    s.p_rp = *rp;
    return 0u;
}

static uint8_t auth_finish(const od_reply_t *rp, uint32_t now)
{
    uint16_t to;

    if ((uint32_t)(now - s.p_ms) > OD_SEC2_HELLO_TO_FINISH_MS) {
        clear_pending(true);
        return OD_SEC_ERR_EXPIRED;
    }
    if (!ct_equal(s.buf, s.u.a.exp_c, 16u)) {
        rate_count(now);
        clear_pending(true);
        return OD_SEC_ERR_AUTH_FAILED;
    }
    session_open(rp, s.u.a.sid, now);
    memcpy(s.buf, s.u.a.tag_d, 16u);
    to = timeout_s();
    s.buf[16] = (uint8_t)to;
    s.buf[17] = (uint8_t)(to >> 8);
    s.len = 18u;
    clear_pending(false);
    return 0u;
}

/* ------------------------------------------------------------------- command handling --- */

static void reply_status(od_tx_reservation_t *r, const od_reply_t *rp)
{
    struct od_sec2_claim_mode cm;
    uint8_t f[OD_SEC2_STATUS_LEN];
    uint8_t flags = 0u;
    uint8_t state;
    uint8_t df = base_dflags();
    const uint16_t to = timeout_s();

    od_sec2_app_claim_mode(&cm);
    if (s.state == OD_SEC2_LOCKED) {
        state = OD_SEC2_STATE_LOCKED;
    } else if (s.state == OD_SEC2_CLAIMED) {
        state = OD_SEC2_STATE_CLAIMED;
    } else {
        state = ((s.pstep == P_CLAIM_BEGUN || s.pstep == P_CLAIM_NONCED) &&
                 od_reply_same(rp, &s.p_rp)) ? OD_SEC2_STATE_CLAIM_PENDING
                                             : OD_SEC2_STATE_UNCLAIMED;
    }
    if (cm.active) {
        flags |= OD_SEC2_SFLAG_CLAIM_MODE;
        if (cm.fresh) {
            flags |= OD_SEC2_SFLAG_CLAIM_MODE_FRESH;
            df |= OD_SEC2_DFLAG_CLAIM_MODE_FRESH;
        }
        if (s.state == OD_SEC2_UNCLAIMED && cm.config_locked) {
            flags |= OD_SEC2_SFLAG_CONFIG_LOCKED;
        }
    } else {
        cm.window_s = 0u;
        cm.renders_left = 0u;
    }
    if (od_sec2_session_live(rp)) {
        flags |= OD_SEC2_SFLAG_SESSION_ACTIVE;
    }
    if (legacy_secrets_open()) {
        flags |= OD_SEC2_SFLAG_LEGACY_SECRETS;
    }
    if (s.state == OD_SEC2_CLAIMED && s.origin == OD_SEC2_ORIGIN_LEGACY) {
        flags |= OD_SEC2_SFLAG_ROTATION_SUGGESTED;
        df |= OD_SEC2_DFLAG_KEY_LEGACY;
    }
    f[0] = RESP_ACK;
    f[1] = (uint8_t)CMD_SEC_STATUS;
    f[2] = OD_SEC2_VERSION;
    f[3] = state;
    f[4] = flags;
    f[5] = (uint8_t)cm.window_s;
    f[6] = (uint8_t)(cm.window_s >> 8);
    od_sec2_app_uid(&f[7]);
    f[15] = (uint8_t)to;
    f[16] = (uint8_t)(to >> 8);
    f[17] = df;
    f[18] = cm.renders_left;
    (void)od_reply_plain(r, rp, f, sizeof f);
}

/* The state-level refusals of sections 8.7 / 9.3, before any reassembly. */
static uint8_t state_check(uint16_t cmd, uint8_t sub)
{
    struct od_sec2_claim_mode cm;

    if (s.state == OD_SEC2_LOCKED) {
        return OD_SEC_ERR_LOCKED;
    }
    if (cmd == CMD_SEC_AUTH) {
        return (s.state == OD_SEC2_CLAIMED) ? 0u : OD_SEC_ERR_NOT_CLAIMED;
    }
    od_sec2_app_claim_mode(&cm);
    if (s.state == OD_SEC2_CLAIMED) {
        return (sub == OD_SEC2_CLAIM_EXIT && cm.active) ? 0u : OD_SEC_ERR_ALREADY_CLAIMED;
    }
    if (!cm.active) {
        return OD_SEC_ERR_NOT_IN_CLAIM_MODE;
    }
    if (cm.renders_left == 0u && sub != OD_SEC2_CLAIM_EXIT &&
        !(sub == OD_SEC2_CLAIM_CONFIRM && s.pstep == P_CLAIM_NONCED)) {
        clear_pending(true);
        od_sec2_app_claim_event(OD_SEC2_EV_EXHAUSTED, 0u);
        return OD_SEC_ERR_ATTEMPTS_EXHAUSTED;
    }
    return 0u;
}

/* Append one fragment. NO_REPLY while the message is incomplete, 0 when it is, else MALFORMED. */
static uint8_t reassemble(uint16_t cmd, uint8_t sub, uint8_t frag, od_span_t chunk, uint32_t now)
{
    const uint8_t idx = (uint8_t)(frag & FRAG_IDX);
    const bool last = (frag & FRAG_LAST) != 0u;

    if (s.rx && (uint32_t)(now - s.rx_ms) > OD_SEC2_FRAGMENT_GAP_MS) {
        s.rx = false;                     /* abandoned: discarded without a reply */
    }
    if (!s.rx) {
        if (idx != 0u || body_len(cmd, sub) == NO_REPLY) {
            return OD_SEC_ERR_MALFORMED;
        }
        s.rx = true;
        s.rx_cmd = cmd;
        s.rx_sub = sub;
        s.rx_next = 0u;
        s.rx_need = body_len(cmd, sub);
        s.len = 0u;
    } else if (cmd != s.rx_cmd || sub != s.rx_sub || idx != s.rx_next) {
        s.rx = false;
        return OD_SEC_ERR_MALFORMED;
    }
    if ((!last && (chunk.n == 0u || idx == FRAG_IDX)) || s.len + chunk.n > s.rx_need) {
        s.rx = false;
        return OD_SEC_ERR_MALFORMED;
    }
    if (chunk.n != 0u) {
        memcpy(&s.buf[s.len], chunk.p, chunk.n);
    }
    s.len = (uint8_t)(s.len + chunk.n);
    s.rx_next++;
    s.rx_ms = now;
    if (!last) {
        return NO_REPLY;
    }
    s.rx = false;
    return (s.len == s.rx_need) ? 0u : OD_SEC_ERR_MALFORMED;
}

static uint8_t process(const od_reply_t *rp, uint16_t cmd, uint8_t sub, uint32_t now)
{
    const bool owner = od_reply_same(rp, &s.p_rp);

    if (cmd == CMD_SEC_AUTH) {
        if (sub == OD_SEC2_AUTH_HELLO) {
            return auth_hello(rp, now);
        }
        if (s.pstep != P_AUTH_HELLOED || !owner) {
            clear_pending(true);
            return OD_SEC_ERR_BAD_STATE;
        }
        return auth_finish(rp, now);
    }
    switch (sub) {
    case OD_SEC2_CLAIM_BEGIN:
        return claim_begin(rp, now);
    case OD_SEC2_CLAIM_NONCE:
        if (s.pstep != P_CLAIM_BEGUN || !owner) {
            return fail_claim(OD_SEC_ERR_BAD_STATE);
        }
        return claim_nonce(now);
    case OD_SEC2_CLAIM_CONFIRM:
        if (s.pstep != P_CLAIM_NONCED || !owner) {
            return fail_claim(OD_SEC_ERR_BAD_STATE);
        }
        return claim_confirm(rp, now);
    case OD_SEC2_CLAIM_ABORT:
        return fail_claim(0u);            /* empty reply, also with nothing pending */
    default:                              /* EXIT */
        clear_pending(true);
        od_sec2_app_claim_event(OD_SEC2_EV_EXIT, 0u);
        return 0u;
    }
}

od_frame_outcome_t od_sec2_command(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                                   od_span_t body)
{
    const uint32_t now = od_sec2_app_now_ms();
    const uint8_t cmd_lo = (uint8_t)cmd;
    const uint8_t sub = (body.n >= 1u) ? body.p[0] : 0u;
    uint8_t err;

    if (cmd == CMD_AUTHENTICATE) {
        uint8_t f[3];
        f[0] = RESP_ACK;
        f[1] = cmd_lo;
        f[2] = (s.state == OD_SEC2_UNCLAIMED) ? AUTH_STATUS_NOT_CONFIG : AUTH_STATUS_V2_REQUIRED;
        (void)od_reply_plain(r, rp, f, sizeof f);
        return OD_FRAME_AUTH_CONTROL;
    }
    if (cmd == CMD_SEC_STATUS) {
        reply_status(r, rp);
        return OD_FRAME_DISCOVERY;
    }
    if (rp->origin != OD_ORIGIN_BLE) {
        err = OD_SEC_ERR_WRONG_TRANSPORT;
    } else if (body.n < 2u) {
        err = OD_SEC_ERR_MALFORMED;
    } else {
        err = state_check(cmd, sub);
        if (err == 0u) {
            err = reassemble(cmd, sub, body.p[1], od_span_drop(body, 2u), now);
            if (err == OD_SEC_ERR_MALFORMED) {
                if (cmd == CMD_SEC_CLAIM) {
                    err = fail_claim(err);
                } else {
                    clear_pending(true);
                }
            } else if (err == 0u) {
                err = process(rp, cmd, sub, now);
                if (err == 0u) {
                    s.tx = true;
                    s.tx_rp = *rp;
                    s.tx_cmd = cmd_lo;
                    s.tx_sub = sub;
                    s.tx_idx = 0u;
                    s.tx_off = 0u;
                    if (s.frag_max < FRAG_MAX_MIN) {
                        s.frag_max = FRAG_MAX_MIN;   /* ABORT/EXIT before any BEGIN: empty */
                    }
                    if (emit_fragment(r) != OD_TXQ_OK) {
                        s.tx = false;
                        s.tx_code = false;
                    }
                    return s.live ? OD_FRAME_AUTH_ESTABLISHED : OD_FRAME_AUTH_CONTROL;
                }
            }
        }
    }
    if (err != NO_REPLY) {
        s.len = 0u;
        s.tx_code = false;
        reply_error(r, rp, cmd_lo, sub, err);
    }
    return OD_FRAME_AUTH_CONTROL;
}

/* ------------------------------------------------------------------------------ SEC_KEY --- */

od_cmd_result_t od_sec2_key_cmd(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const uint8_t sub = (body.n >= 1u) ? body.p[0] : 0u;
    uint8_t f[3u + 16u];
    uint8_t kconf[16];
    uint8_t ctx24[OD_SEC2_UID_LEN + 16u];
    uint8_t tag[16];
    uint8_t err = 0u;
    uint8_t n = 3u;
    bool teardown = false;
    enum { KEEP, ROTATED, UNCLAIMED } after = KEEP;
    const uint16_t gen = (uint16_t)(s.gen + 1u);

    if (ctx->rp.origin != OD_ORIGIN_BLE) {
        err = OD_SEC_ERR_WRONG_TRANSPORT;
    } else if (s.state != OD_SEC2_CLAIMED || !s.live) {
        err = OD_SEC_ERR_NOT_CLAIMED;
    } else if (sub == OD_SEC2_KEY_ROTATE && body.n == 1u) {
        if (od_hal_crypto_random(s.u.c.k_new, 16u) != OD_HAL_CRYPTO_OK) {
            err = OD_SEC_ERR_CRYPTO_ERROR;
        } else {
            s.pstep = P_ROTATE;
            memcpy(&f[3], s.u.c.k_new, 16u);
            n = 19u;
        }
    } else if (sub == OD_SEC2_KEY_ROTATE_CONFIRM && body.n == 17u) {
        if (s.pstep != P_ROTATE) {
            err = OD_SEC_ERR_BAD_STATE;
        } else {
            od_sec2_app_uid(ctx24);
            memcpy(&ctx24[OD_SEC2_UID_LEN], s.sid, 16u);
            if (od_sec2_kdf(s.u.c.k_new, "OD2/k-conf", ctx24, OD_SEC2_UID_LEN, kconf) !=
                    OD_HAL_CRYPTO_OK ||
                od_sec2_kdf(kconf, "OD2/rotate-c", ctx24, sizeof ctx24, tag) != OD_HAL_CRYPTO_OK) {
                err = OD_SEC_ERR_CRYPTO_ERROR;
            } else if (!ct_equal(tag, body.p + 1, 16u)) {
                err = OD_SEC_ERR_AUTH_FAILED;
            } else if (!write_os2k(OD_SEC2_ORIGIN_CLAIMED, gen, s.u.c.k_new)) {
                err = OD_SEC_ERR_STORAGE_ERROR;
            } else {
                after = ROTATED;
            }
            if (err != 0u) {
                s.pstep = P_NONE;
                wipe(&s.u, sizeof s.u);
            }
        }
    } else if (sub == OD_SEC2_KEY_UNCLAIM && body.n == 3u && body.p[1] == 0xA5u &&
               body.p[2] == 0x5Au) {
        if (!od_sec2_app_scrub() || !od_hal_secret_erase(OD_HAL_SECRET_KEY)) {
            err = OD_SEC_ERR_STORAGE_ERROR;
        } else {
            after = UNCLAIMED;
        }
    } else {
        err = OD_SEC_ERR_MALFORMED;
    }

    if (after != KEEP) {
        s.os2s.key_gen = gen;             /* rotation: the new gen; unclaim: a tombstone */
        (void)write_os2s();
        teardown = true;
    }
    f[0] = (err != 0u) ? RESP_NACK : RESP_ACK;
    f[1] = (uint8_t)CMD_SEC_KEY;
    f[2] = sub;
    if (err != 0u) {
        f[3] = err;
        n = 4u;
    }
    /* Sealed under the CURRENT session, so before the RAM state moves. */
    (void)od_reply(ctx->r, &ctx->rp, f, n);
    if (after == ROTATED) {
        set_key(OD_SEC2_ORIGIN_CLAIMED, gen, s.u.c.k_new);
        od_sec2_app_rest_sync();
    } else if (after == UNCLAIMED) {
        s.state = OD_SEC2_UNCLAIMED;
        wipe(s.k_dev, sizeof s.k_dev);
    }
    if (teardown) {
        s.pstep = P_NONE;
        wipe(&s.u, sizeof s.u);
        od_sec2_teardown();
    }
    wipe(f, sizeof f);
    wipe(kconf, sizeof kconf);
    wipe(tag, sizeof tag);
    return (err != 0u) ? OD_CMD_NACK : OD_CMD_OK;
}
