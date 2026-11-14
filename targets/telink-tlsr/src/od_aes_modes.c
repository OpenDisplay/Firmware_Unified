#include "od_aes_modes.h"

#include <string.h>

static void xor16(uint8_t *dst, const uint8_t *src, uint8_t n)
{
    uint8_t i;
    for (i = 0; i < n; ++i) {
        dst[i] ^= src[i];
    }
}

/* ------------------------------------------------------------------------------ CMAC --- */

/* GF(2^128) doubling for the CMAC subkeys (RFC 4493 section 2.3). */
static void cmac_dbl(uint8_t b[16])
{
    uint8_t carry = (uint8_t)(b[0] >> 7);
    uint8_t i;
    for (i = 0; i < 15u; ++i) {
        b[i] = (uint8_t)((b[i] << 1) | (b[i + 1u] >> 7));
    }
    b[15] = (uint8_t)((b[15] << 1) ^ (carry ? 0x87u : 0u));
}

bool od_aes_cmac(od_aes_block_fn enc, const uint8_t key[16], const uint8_t *msg, uint32_t len,
                 uint8_t out[16])
{
    uint8_t k[16] = { 0 };
    uint8_t x[16] = { 0 };
    uint8_t last[16];
    uint32_t n_blocks, i;
    uint32_t tail;

    if (enc == NULL || key == NULL || out == NULL || (msg == NULL && len != 0u)) {
        return false;
    }
    if (!enc(key, k, k)) {
        return false;
    }
    cmac_dbl(k);                                   /* K1 */
    n_blocks = (len + 15u) / 16u;
    tail = len - (n_blocks ? (n_blocks - 1u) * 16u : 0u);
    if (n_blocks == 0u || tail != 16u) {
        cmac_dbl(k);                               /* K2: last block is padded */
        memset(last, 0, sizeof(last));
        if (n_blocks == 0u) {
            n_blocks = 1u;
            tail = 0u;
        } else {
            memcpy(last, msg + (n_blocks - 1u) * 16u, tail);
        }
        last[tail] = 0x80u;
    } else {
        memcpy(last, msg + (n_blocks - 1u) * 16u, 16u);
    }
    xor16(last, k, 16u);

    for (i = 0; i + 1u < n_blocks; ++i) {
        xor16(x, msg + i * 16u, 16u);
        if (!enc(key, x, x)) {
            return false;
        }
    }
    xor16(x, last, 16u);
    if (!enc(key, x, out)) {
        return false;
    }
    memset(k, 0, sizeof(k));
    return true;
}

/* ------------------------------------------------------------------------------- CCM --- */

static bool ccm_params_ok(uint8_t nonce_len, uint8_t tag_len, uint16_t aad_len)
{
    return nonce_len >= 7u && nonce_len <= 13u && tag_len >= 4u && tag_len <= 16u &&
           (tag_len & 1u) == 0u && aad_len < 0xFF00u;
}

/* A_i / B_0 share the layout flags | nonce | big-endian counter-or-length in L bytes. */
static void ccm_block(uint8_t blk[16], uint8_t flags, const uint8_t *nonce, uint8_t nonce_len,
                      uint32_t value)
{
    uint8_t i;
    memset(blk, 0, 16u);
    blk[0] = flags;
    memcpy(&blk[1], nonce, nonce_len);
    for (i = 15u; i > nonce_len && value != 0u; --i) {
        blk[i] = (uint8_t)value;
        value >>= 8;
    }
}

/* CBC-MAC over B_0, the length-prefixed AAD and the plaintext, each zero-padded to 16 bytes. */
static bool ccm_mac(od_aes_block_fn enc, const uint8_t key[16], const uint8_t *nonce,
                    uint8_t nonce_len, const uint8_t *aad, uint16_t aad_len,
                    const uint8_t *plain, uint16_t plain_len, uint8_t tag_len, uint8_t x[16])
{
    uint8_t L = (uint8_t)(15u - nonce_len);
    uint8_t flags = (uint8_t)((aad_len ? 0x40u : 0u) | (((tag_len - 2u) / 2u) << 3) | (L - 1u));
    uint8_t blk[16];
    uint16_t off;
    uint8_t n;

    ccm_block(x, flags, nonce, nonce_len, plain_len);
    if (!enc(key, x, x)) {
        return false;
    }
    if (aad_len) {
        memset(blk, 0, sizeof(blk));
        blk[0] = (uint8_t)(aad_len >> 8);
        blk[1] = (uint8_t)aad_len;
        n = (uint8_t)(aad_len < 14u ? aad_len : 14u);
        memcpy(&blk[2], aad, n);
        xor16(x, blk, 16u);
        if (!enc(key, x, x)) {
            return false;
        }
        for (off = n; off < aad_len; off = (uint16_t)(off + n)) {
            n = (uint8_t)((aad_len - off) < 16u ? (aad_len - off) : 16u);
            xor16(x, aad + off, n);
            if (!enc(key, x, x)) {
                return false;
            }
        }
    }
    for (off = 0; off < plain_len; off = (uint16_t)(off + n)) {
        n = (uint8_t)((plain_len - off) < 16u ? (plain_len - off) : 16u);
        xor16(x, plain + off, n);
        if (!enc(key, x, x)) {
            return false;
        }
    }
    return true;
}

/* CTR keystream from A_1 onward; in and out may alias. */
static bool ccm_ctr(od_aes_block_fn enc, const uint8_t key[16], const uint8_t *nonce,
                    uint8_t nonce_len, const uint8_t *in, uint16_t len, uint8_t *out)
{
    uint8_t L = (uint8_t)(15u - nonce_len);
    uint8_t a[16], s[16];
    uint16_t off;
    uint32_t ctr = 1u;
    uint8_t n, i;

    for (off = 0; off < len; off = (uint16_t)(off + n), ++ctr) {
        ccm_block(a, (uint8_t)(L - 1u), nonce, nonce_len, ctr);
        if (!enc(key, a, s)) {
            return false;
        }
        n = (uint8_t)((len - off) < 16u ? (len - off) : 16u);
        for (i = 0; i < n; ++i) {
            out[off + i] = (uint8_t)(in[off + i] ^ s[i]);
        }
    }
    return true;
}

static bool ccm_s0(od_aes_block_fn enc, const uint8_t key[16], const uint8_t *nonce,
                   uint8_t nonce_len, uint8_t s0[16])
{
    uint8_t a0[16];
    ccm_block(a0, (uint8_t)(15u - nonce_len - 1u), nonce, nonce_len, 0u);
    return enc(key, a0, s0);
}

bool od_aes_ccm_encrypt(od_aes_block_fn enc, const uint8_t key[16],
                        const uint8_t *nonce, uint8_t nonce_len,
                        const uint8_t *aad, uint16_t aad_len,
                        const uint8_t *plain, uint16_t plain_len,
                        uint8_t tag_len, uint8_t *out)
{
    uint8_t t[16], s0[16];

    if (enc == NULL || key == NULL || nonce == NULL || out == NULL ||
        !ccm_params_ok(nonce_len, tag_len, aad_len) ||
        (aad == NULL && aad_len) || (plain == NULL && plain_len)) {
        return false;
    }
    /* MAC first: `out` may alias `plain`, and the CTR pass overwrites it. */
    if (!ccm_mac(enc, key, nonce, nonce_len, aad, aad_len, plain, plain_len, tag_len, t) ||
        !ccm_s0(enc, key, nonce, nonce_len, s0) ||
        !ccm_ctr(enc, key, nonce, nonce_len, plain, plain_len, out)) {
        return false;
    }
    xor16(t, s0, tag_len);
    memcpy(out + plain_len, t, tag_len);
    return true;
}

bool od_aes_ccm_decrypt(od_aes_block_fn enc, const uint8_t key[16],
                        const uint8_t *nonce, uint8_t nonce_len,
                        const uint8_t *aad, uint16_t aad_len,
                        const uint8_t *in, uint16_t ct_len,
                        uint8_t tag_len, uint8_t *plain, bool *auth_ok)
{
    uint8_t t[16], s0[16], rx_tag[16];
    uint16_t plain_len;
    uint8_t diff = 0u, i;

    if (auth_ok != NULL) {
        *auth_ok = false;
    }
    if (enc == NULL || key == NULL || nonce == NULL || in == NULL || plain == NULL ||
        auth_ok == NULL || !ccm_params_ok(nonce_len, tag_len, aad_len) ||
        (aad == NULL && aad_len) || ct_len < tag_len) {
        return false;
    }
    plain_len = (uint16_t)(ct_len - tag_len);
    memcpy(rx_tag, in + plain_len, tag_len);       /* before `plain` may overwrite it */
    if (!ccm_ctr(enc, key, nonce, nonce_len, in, plain_len, plain) ||
        !ccm_mac(enc, key, nonce, nonce_len, aad, aad_len, plain, plain_len, tag_len, t) ||
        !ccm_s0(enc, key, nonce, nonce_len, s0)) {
        memset(plain, 0, plain_len);
        return false;
    }
    xor16(t, s0, tag_len);
    for (i = 0; i < tag_len; ++i) {                /* constant time over the tag */
        diff |= (uint8_t)(t[i] ^ rx_tag[i]);
    }
    if (diff != 0u) {
        memset(plain, 0, plain_len);
        return true;
    }
    *auth_ok = true;
    return true;
}
