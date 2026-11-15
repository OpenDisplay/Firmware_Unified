/* sec2_client.c -- see sec2_client.h. NOT INDEPENDENT of shared/core/od_sec2.c. */

#include "sec2_client.h"

#include "od_sec2.h"
#include "session_fake.h"

#include <string.h>

void sec2c_init(struct sec2c *c, const uint8_t k_dev[16], const uint8_t uid[8])
{
    memset(c, 0, sizeof *c);
    if (k_dev != NULL) { memcpy(c->k_dev, k_dev, 16u); }
    memcpy(c->uid, uid, 8u);
    c->frag_max = 20u;
}

void sec2c_kdf(const uint8_t k[16], const char *label, const uint8_t *ctx, uint16_t ctx_len,
               uint8_t out[16])
{
    uint8_t in[256];
    size_t n = 0u;
    const size_t ll = strlen(label);

    in[n++] = 0x01u;
    memcpy(&in[n], label, ll); n += ll;
    in[n++] = 0x00u;
    if (ctx_len != 0u) { memcpy(&in[n], ctx, ctx_len); n += ctx_len; }
    in[n++] = 0x00u;
    in[n++] = 0x80u;
    host_cmac(k, in, (uint32_t)n, out);
}

unsigned sec2c_send(const od_reply_t *rp, const uint8_t *frame, uint16_t n,
                    uint8_t frames[][256], uint16_t *lens, unsigned max)
{
    unsigned got = 0u;
    unsigned guard;
    uint8_t buf[256];
    uint16_t len;

    (void)sec2c_deliver(rp, od_span_make(frame, n));
    for (guard = 0u; guard < 64u; ++guard) {
        (void)od_txq_process();
        while (sec2c_radio_pop(buf, &len)) {
            if (got < max) {
                memcpy(frames[got], buf, len);
                lens[got] = len;
            }
            ++got;
        }
        if (!od_sec2_tx_active() && od_txq_depth() == 0u) { break; }
        od_sec2_pump();
    }
    return got;
}

int sec2c_xchg(struct sec2c *c, const od_reply_t *rp, uint16_t cmd, uint8_t sub,
               const uint8_t *body, uint8_t len, uint8_t *resp, uint8_t resp_cap)
{
    const uint8_t room = (uint8_t)(c->frag_max - 4u);
    uint8_t frames[16][256];
    uint16_t lens[16];
    uint8_t f[256];
    uint8_t off = 0u;
    uint8_t idx = 0u;
    unsigned n = 0u;
    unsigned i;
    int out = 0;

    c->last_err = 0u;
    do {
        const uint8_t take = (uint8_t)((len - off) < room ? (len - off) : room);
        const bool last = (uint8_t)(off + take) == len;

        f[0] = (uint8_t)(cmd >> 8);
        f[1] = (uint8_t)cmd;
        f[2] = sub;
        f[3] = (uint8_t)(idx | (last ? 0x80u : 0u));
        if (take != 0u) { memcpy(&f[4], &body[off], take); }
        n = sec2c_send(rp, f, (uint16_t)(4u + take), frames, lens, 16u);
        off = (uint8_t)(off + take);
        ++idx;
        if (!last && n != 0u) { break; }          /* an error ends the message early */
    } while (off < len);

    if (n == 1u && lens[0] == 4u && frames[0][0] == 0xFFu) {
        c->last_err = frames[0][3];
        return -(int)frames[0][3];
    }
    for (i = 0u; i < n && i < 16u; ++i) {
        const uint8_t *p = frames[i];
        if (lens[i] < 4u || p[0] != 0x00u || p[1] != (uint8_t)cmd || p[2] != sub ||
            (p[3] & 0x7Fu) != i) {
            return -1000;
        }
        if (out + (lens[i] - 4) > resp_cap) { return -1000; }
        memcpy(&resp[out], &p[4], lens[i] - 4u);
        out += lens[i] - 4;
        if ((p[3] & 0x80u) != 0u) { return (i + 1u == n) ? out : -1000; }
    }
    return -1000;
}

/* Transcript: prefix || 02 || dflags || uid || Qc || Qd || Nc || Nd. */
static uint16_t transcript(uint8_t *t, const char *prefix, uint8_t dflags, const uint8_t uid[8],
                           const uint8_t qc[64], const uint8_t qd[64], const uint8_t nc[16],
                           const uint8_t nd[16])
{
    uint16_t n = (uint16_t)strlen(prefix);

    memcpy(t, prefix, n);
    t[n++] = 0x02u;
    t[n++] = dflags;
    memcpy(&t[n], uid, 8u);  n += 8u;
    memcpy(&t[n], qc, 64u);  n += 64u;
    memcpy(&t[n], qd, 64u);  n += 64u;
    memcpy(&t[n], nc, 16u);  n += 16u;
    memcpy(&t[n], nd, 16u);  n += 16u;
    return n;
}

static void session_keys(struct sec2c *c, const uint8_t prk[16], const uint8_t *t, uint16_t tn,
                         const uint8_t *tail)
{
    sec2c_kdf(prk, "OD2/h2d", t, tn, c->k_h2d);
    sec2c_kdf(prk, "OD2/d2h", t, tn, c->k_d2h);
    sec2c_kdf(prk, "OD2/sid", t, tn, c->sid);
    c->tx_ctr = 0u;
    c->rx_ctr = 0u;
    c->timeout_s = (uint16_t)(tail[0] | (tail[1] << 8));
    c->live = true;
}

int sec2c_auth(struct sec2c *c, const od_reply_t *rp)
{
    uint8_t body[82], r[96], qc[64], nc[16], z[32], k[16], ta[178], tag[16];
    void *key;
    int n;
    uint16_t tn;

    c->live = false;
    key = tp256_new(qc);
    memset(nc, 0xC5, sizeof nc);
    body[0] = 0x02u;
    body[1] = c->frag_max;
    memcpy(&body[2], qc, 64u);
    memcpy(&body[66], nc, 16u);
    n = sec2c_xchg(c, rp, CMD_SEC_AUTH, OD_SEC2_AUTH_HELLO, body, 82u, r, sizeof r);
    if (n < 0) { tp256_free(key); return (n == -1000) ? 1000 : -n; }
    if (n != 81 || !tp256_agree(key, &r[1], z)) { tp256_free(key); return 1000; }
    tp256_free(key);
    c->dflags = r[0];
    tn = transcript(ta, "OD2/auth", r[0], c->uid, qc, &r[1], nc, &r[65]);
    sec2c_kdf(c->k_dev, "OD2/k-ext", c->uid, 8u, k);
    host_cmac(k, z, 32u, k);                          /* PRK_auth = CMAC(K_ext, Z) */
    sec2c_kdf(k, "OD2/auth-c", ta, tn, tag);
    n = sec2c_xchg(c, rp, CMD_SEC_AUTH, OD_SEC2_AUTH_FINISH, tag, 16u, r, sizeof r);
    if (n < 0) { return (n == -1000) ? 1000 : -n; }
    sec2c_kdf(k, "OD2/auth-d", ta, tn, tag);
    if (n != 18 || memcmp(tag, r, 16u) != 0) { return 1000; }
    session_keys(c, k, ta, tn, &r[16]);
    return 0;
}

int sec2c_claim(struct sec2c *c, const od_reply_t *rp, bool corrupt_tag)
{
    uint8_t body[66], r[96], qc[64], qd[64], cd[16], nd[16], nc[16], z[32], prk[16], kw[16];
    uint8_t tc[179], tag[16], kconf[16], knew[16], in[160], aad[16];
    static const uint8_t zero13[13];
    void *key;
    int n;
    uint16_t tn;
    uint8_t dflags;

    c->live = false;
    key = tp256_new(qc);
    body[0] = 0x02u;
    body[1] = c->frag_max;
    memcpy(&body[2], qc, 64u);
    n = sec2c_xchg(c, rp, CMD_SEC_CLAIM, OD_SEC2_CLAIM_BEGIN, body, 66u, r, sizeof r);
    if (n < 0) { tp256_free(key); return (n == -1000) ? 1000 : -n; }
    if (n != 81) { tp256_free(key); return 1000; }
    dflags = r[0];
    c->dflags = dflags;
    memcpy(qd, &r[1], 64u);
    memcpy(cd, &r[65], 16u);
    memset(nc, 0x3C, sizeof nc);
    n = sec2c_xchg(c, rp, CMD_SEC_CLAIM, OD_SEC2_CLAIM_NONCE, nc, 16u, r, sizeof r);
    if (n < 0) { tp256_free(key); return (n == -1000) ? 1000 : -n; }
    if (n != 44 || !tp256_agree(key, qd, z)) { tp256_free(key); return 1000; }
    tp256_free(key);
    memcpy(nd, r, 16u);
    /* the commitment opens */
    memcpy(in, "OD2/commit", 10u);
    memcpy(&in[10], c->uid, 8u);
    memcpy(&in[18], qd, 64u);
    memcpy(&in[82], qc, 64u);
    host_cmac(nd, in, 146u, tag);
    if (memcmp(tag, cd, 16u) != 0) { return 1000; }
    tn = transcript(tc, "OD2/claim", dflags, c->uid, qc, qd, nc, nd);
    host_cmac((const uint8_t[16]){0}, z, 32u, prk);
    sec2c_kdf(prk, "OD2/wrap", tc, tn, kw);
    memcpy(aad, "OD2/wrap", 8u);
    memcpy(&aad[8], c->uid, 8u);
    if (!host_ccm_decrypt(kw, zero13, aad, 16u, &r[16], 28u, knew)) { return 1000; }
    /* the code */
    memcpy(in, "OD2/code", 8u);
    memcpy(&in[8], c->uid, 8u);
    memcpy(&in[16], qc, 64u);
    memcpy(&in[80], qd, 64u);
    memcpy(&in[144], nd, 16u);
    host_cmac(nc, in, 160u, tag);
    c->code = (((uint32_t)tag[12] << 24) | ((uint32_t)tag[13] << 16) |
               ((uint32_t)tag[14] << 8) | tag[15]) % 1000000u;
    sec2c_kdf(knew, "OD2/k-conf", c->uid, 8u, kconf);
    sec2c_kdf(kconf, "OD2/claim-c", tc, tn, tag);
    if (corrupt_tag) { tag[0] ^= 0x01u; }
    n = sec2c_xchg(c, rp, CMD_SEC_CLAIM, OD_SEC2_CLAIM_CONFIRM, tag, 16u, r, sizeof r);
    if (n < 0) { return (n == -1000) ? 1000 : -n; }
    sec2c_kdf(kconf, "OD2/claim-d", tc, tn, tag);
    if (n != 18 || memcmp(tag, r, 16u) != 0) { return 1000; }
    memcpy(c->k_dev, knew, 16u);
    session_keys(c, prk, tc, tn, &r[16]);
    return 0;
}

uint16_t sec2c_seal_ctr(struct sec2c *c, uint16_t cmd, uint32_t ctr, const uint8_t *payload,
                        uint16_t n, uint8_t *out)
{
    uint8_t nonce[13] = {0};
    uint8_t aad[3];

    nonce[0] = 0x48u;
    nonce[1] = (uint8_t)ctr; nonce[2] = (uint8_t)(ctr >> 8);
    nonce[3] = (uint8_t)(ctr >> 16); nonce[4] = (uint8_t)(ctr >> 24);
    aad[0] = 0x02u;
    aad[1] = (uint8_t)(cmd >> 8);
    aad[2] = (uint8_t)cmd;
    out[0] = aad[1];
    out[1] = aad[2];
    memcpy(&out[2], &nonce[1], 4u);
    host_ccm_encrypt(c->k_h2d, nonce, aad, 3u, payload, n, &out[6]);
    return (uint16_t)(6u + n + 12u);
}

uint16_t sec2c_seal(struct sec2c *c, uint16_t cmd, const uint8_t *payload, uint16_t n,
                    uint8_t *out)
{
    return sec2c_seal_ctr(c, cmd, ++c->tx_ctr, payload, n, out);
}

bool sec2c_open(struct sec2c *c, const uint8_t *frame, uint16_t n, uint8_t *out,
                uint16_t *out_n)
{
    static const uint8_t aad[2] = { 0x02u, 0xE5u };
    uint8_t nonce[13] = {0};
    uint32_t ctr;

    if (n < 17u || frame[0] != 0xE5u) { return false; }
    ctr = (uint32_t)frame[1] | ((uint32_t)frame[2] << 8) | ((uint32_t)frame[3] << 16) |
          ((uint32_t)frame[4] << 24);
    if (ctr <= c->rx_ctr) { return false; }
    nonce[0] = 0x44u;
    memcpy(&nonce[1], &frame[1], 4u);
    if (!host_ccm_decrypt(c->k_d2h, nonce, aad, 2u, &frame[5], (uint16_t)(n - 5u), out)) {
        return false;
    }
    c->rx_ctr = ctr;
    *out_n = (uint16_t)(n - 17u);
    return true;
}
