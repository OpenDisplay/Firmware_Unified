/* od_hal_crypto over a single AES-128 block primitive: CCM (RFC 3610 / SP 800-38C, 12-byte
 * tag) and CMAC (RFC 4493) in software, P-256 ECDH and random from the backend. Plain C, no SDK
 * header; tests/host/tlsr_crypto_test.c checks it against the RFC vectors and the HAL contract.
 *
 * Tag comparison is constant-time. The AES block itself is the chip's hardware engine on the
 * target, so no key-dependent table lookup happens here. */
#include "od_hal_crypto.h"

#include "od_sec2.h"
#include "od_tlsr_crypto_backend.h"

#include <string.h>

static uint8_t s_key[OD_HAL_CRYPTO_KEY_SLOTS][16];
static bool    s_key_set[OD_HAL_CRYPTO_KEY_SLOTS];
static uint8_t s_ecdh_priv[32];
static bool    s_ecdh_ready;

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n-- != 0u) {
        *v++ = 0u;
    }
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot, const uint8_t key[16])
{
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || key == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    memcpy(s_key[slot], key, 16u);
    s_key_set[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    if (slot < OD_HAL_CRYPTO_KEY_SLOTS) {
        wipe(s_key[slot], 16u);
        s_key_set[slot] = false;
    }
}

/* ---- CCM ----
 * Flags/B0: Adata | ((M-2)/2)<<3 | (L-1), nonce, message length in L bytes. A_i: (L-1), nonce,
 * counter i. The MAC is over B0, the AAD block(s) (2-byte length prefix; aad_len < 0xFF00
 * always holds for a uint8_t length) and the zero-padded plaintext. */

static bool ccm_check_args(od_hal_crypto_slot_t slot, const uint8_t *nonce, uint8_t nonce_len,
                           const uint8_t *aad, uint8_t aad_len)
{
    return slot < OD_HAL_CRYPTO_KEY_SLOTS && s_key_set[slot] && nonce != NULL &&
           nonce_len >= 7u && nonce_len <= 13u && (aad != NULL || aad_len == 0u);
}

static void ccm_ctr_block(uint8_t a[16], const uint8_t *nonce, uint8_t nonce_len, uint32_t i)
{
    uint8_t L = (uint8_t)(15u - nonce_len);
    unsigned k;

    memset(a, 0, 16u);
    a[0] = (uint8_t)(L - 1u);
    memcpy(&a[1], nonce, nonce_len);
    for (k = 0u; k < L && k < 4u; ++k) {
        a[15u - k] = (uint8_t)(i >> (8u * k));
    }
}

static void xor16(uint8_t *x, const uint8_t *y, size_t n)
{
    size_t k;
    for (k = 0u; k < n; ++k) {
        x[k] ^= y[k];
    }
}

static bool ccm_mac(const uint8_t key[16], const uint8_t *nonce, uint8_t nonce_len,
                    const uint8_t *aad, uint8_t aad_len, const uint8_t *msg, uint16_t msg_len,
                    uint8_t tag[16])
{
    uint8_t b[16];
    uint8_t L = (uint8_t)(15u - nonce_len);
    unsigned k;
    size_t off, n;

    memset(b, 0, sizeof b);
    b[0] = (uint8_t)((aad_len != 0u ? 0x40u : 0u) | (((OD_HAL_CRYPTO_TAG_LEN - 2u) / 2u) << 3) |
                     (L - 1u));
    memcpy(&b[1], nonce, nonce_len);
    for (k = 0u; k < L && k < 2u; ++k) {
        b[15u - k] = (uint8_t)(msg_len >> (8u * k));
    }
    if (!od_tlsr_aes128(key, b, tag)) {
        return false;
    }
    if (aad_len != 0u) {
        /* first block: 2-byte length then up to 14 AAD bytes; then 16-byte blocks */
        memset(b, 0, sizeof b);
        b[0] = 0u;
        b[1] = aad_len;
        n = aad_len < 14u ? aad_len : 14u;
        memcpy(&b[2], aad, n);
        xor16(tag, b, 16u);
        if (!od_tlsr_aes128(key, tag, tag)) {
            return false;
        }
        for (off = n; off < aad_len; off += 16u) {
            size_t m = aad_len - off < 16u ? aad_len - off : 16u;
            xor16(tag, aad + off, m);
            if (!od_tlsr_aes128(key, tag, tag)) {
                return false;
            }
        }
    }
    for (off = 0u; off < msg_len; off += 16u) {
        size_t m = msg_len - off < 16u ? msg_len - off : 16u;
        xor16(tag, msg + off, m);
        if (!od_tlsr_aes128(key, tag, tag)) {
            return false;
        }
    }
    wipe(b, sizeof b);
    return true;
}

/* out = in XOR keystream(counter 1..), in place allowed. */
static bool ccm_ctr(const uint8_t key[16], const uint8_t *nonce, uint8_t nonce_len,
                    const uint8_t *in, uint8_t *out, uint16_t len)
{
    uint8_t a[16], s[16];
    uint32_t i = 1u;
    size_t off;

    for (off = 0u; off < len; off += 16u, ++i) {
        size_t m = len - off < 16u ? len - off : 16u;
        size_t k;
        ccm_ctr_block(a, nonce, nonce_len, i);
        if (!od_tlsr_aes128(key, a, s)) {
            return false;
        }
        for (k = 0u; k < m; ++k) {
            out[off + k] = (uint8_t)(in[off + k] ^ s[k]);
        }
    }
    wipe(s, sizeof s);
    return true;
}

static bool ccm_tag_mask(const uint8_t key[16], const uint8_t *nonce, uint8_t nonce_len,
                         uint8_t tag[16])
{
    uint8_t a[16], s0[16];

    ccm_ctr_block(a, nonce, nonce_len, 0u);
    if (!od_tlsr_aes128(key, a, s0)) {
        return false;
    }
    xor16(tag, s0, 16u);
    wipe(s0, sizeof s0);
    return true;
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len, uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len)
{
    uint8_t tag[16];

    if (!ccm_check_args(slot, nonce, nonce_len, aad, aad_len) || plain == NULL || ct == NULL ||
        ct_len == NULL || (uint32_t)plain_len + OD_HAL_CRYPTO_TAG_LEN > ct_cap) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (!ccm_mac(s_key[slot], nonce, nonce_len, aad, aad_len, plain, plain_len, tag) ||
        !ccm_tag_mask(s_key[slot], nonce, nonce_len, tag) ||
        !ccm_ctr(s_key[slot], nonce, nonce_len, plain, ct, plain_len)) {
        wipe(tag, sizeof tag);
        return OD_HAL_CRYPTO_ERROR;
    }
    memcpy(ct + plain_len, tag, OD_HAL_CRYPTO_TAG_LEN);
    *ct_len = (uint16_t)(plain_len + OD_HAL_CRYPTO_TAG_LEN);
    wipe(tag, sizeof tag);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len, uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len)
{
    uint8_t tag[16];
    uint16_t n;
    uint8_t diff = 0u;
    unsigned k;

    if (!ccm_check_args(slot, nonce, nonce_len, aad, aad_len) || ct == NULL ||
        ct_len < OD_HAL_CRYPTO_TAG_LEN || plain == NULL || plain_len == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    n = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    if (plain_cap < n) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (!ccm_ctr(s_key[slot], nonce, nonce_len, ct, plain, n) ||
        !ccm_mac(s_key[slot], nonce, nonce_len, aad, aad_len, plain, n, tag) ||
        !ccm_tag_mask(s_key[slot], nonce, nonce_len, tag)) {
        wipe(plain, n);
        wipe(tag, sizeof tag);
        return OD_HAL_CRYPTO_ERROR;
    }
    for (k = 0u; k < OD_HAL_CRYPTO_TAG_LEN; ++k) {
        diff |= (uint8_t)(tag[k] ^ ct[n + k]);
    }
    wipe(tag, sizeof tag);
    if (diff != 0u) {
        wipe(plain, n);
        return OD_HAL_CRYPTO_AUTH_FAILED;
    }
    *plain_len = n;
    return OD_HAL_CRYPTO_OK;
}

/* ---- CMAC (RFC 4493) ---- */

static void dbl(uint8_t b[16])
{
    uint8_t carry = (uint8_t)(b[0] >> 7);
    unsigned k;

    for (k = 0u; k < 15u; ++k) {
        b[k] = (uint8_t)((b[k] << 1) | (b[k + 1u] >> 7));
    }
    b[15] = (uint8_t)((b[15] << 1) ^ (carry != 0u ? 0x87u : 0u));
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    uint8_t k1[16], x[16], last[16];
    uint32_t nblk, i;
    bool complete;

    if (key == NULL || out == NULL || (msg == NULL && msg_len != 0u)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    memset(x, 0, sizeof x);
    if (!od_tlsr_aes128(key, x, k1)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    dbl(k1); /* K1 */
    nblk = (msg_len + 15u) / 16u;
    complete = nblk != 0u && (msg_len % 16u) == 0u;
    if (nblk == 0u) {
        nblk = 1u;
    }
    for (i = 0u; i + 1u < nblk; ++i) {
        xor16(x, msg + 16u * i, 16u);
        if (!od_tlsr_aes128(key, x, x)) {
            return OD_HAL_CRYPTO_ERROR;
        }
    }
    memset(last, 0, sizeof last);
    if (complete) {
        memcpy(last, msg + 16u * (nblk - 1u), 16u);
    } else {
        uint32_t r = msg_len - 16u * (nblk - 1u);
        if (r != 0u) {
            memcpy(last, msg + 16u * (nblk - 1u), r);
        }
        last[r] = 0x80u;
        dbl(k1); /* K2 */
    }
    xor16(last, k1, 16u);
    xor16(x, last, 16u);
    if (!od_tlsr_aes128(key, x, out)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    wipe(k1, sizeof k1);
    wipe(x, sizeof x);
    wipe(last, sizeof last);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    if (key == NULL || in == NULL || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    return od_tlsr_aes128(key, in, out) ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_ERROR;
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    if (buf == NULL && len != 0u) {
        return OD_HAL_CRYPTO_ERROR;
    }
    return (len == 0u || od_tlsr_random(buf, len)) ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_ERROR;
}

/* ---- P-256 ECDH: one key pair per handshake, wiped by agree or discard ---- */

void od_hal_crypto_ecdh_discard(void)
{
    wipe(s_ecdh_priv, sizeof s_ecdh_priv);
    s_ecdh_ready = false;
}

enum od_hal_crypto_status od_hal_crypto_ecdh_keygen(uint8_t pub[64])
{
    if (pub == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    od_hal_crypto_ecdh_discard();
    if (!od_tlsr_p256_keygen(pub, s_ecdh_priv) || !od_sec2_point_valid(pub)) {
        od_hal_crypto_ecdh_discard();
        return OD_HAL_CRYPTO_ERROR;
    }
    s_ecdh_ready = true;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ecdh_agree(const uint8_t peer[64], uint8_t z[32])
{
    enum od_hal_crypto_status rc;

    if (!s_ecdh_ready || peer == NULL || z == NULL) {
        od_hal_crypto_ecdh_discard();
        return OD_HAL_CRYPTO_ERROR;
    }
    if (!od_sec2_point_valid(peer)) {
        rc = OD_HAL_CRYPTO_BAD_POINT;
    } else {
        rc = od_tlsr_p256_agree(peer, s_ecdh_priv, z) ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_ERROR;
    }
    od_hal_crypto_ecdh_discard();
    return rc;
}
