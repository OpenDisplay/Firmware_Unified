/* session_fake.c -- see session_fake.h. One fake for the unit suites and tests/fuzz/ alike, so a
 * fuzz finding reproduces in a suite. */

#define OPENSSL_SUPPRESS_DEPRECATED 1

#include "session_fake.h"

#include "aes128.h"

#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/obj_mac.h>

#include <string.h>

/* --------------------------------------------------------------------- AES-CMAC on the host --- */

static void cmac_dbl(uint8_t b[16])
{
    uint8_t carry = (uint8_t)(b[0] >> 7);
    unsigned i;

    for (i = 0; i < 15u; ++i) {
        b[i] = (uint8_t)((b[i] << 1) | (b[i + 1u] >> 7));
    }
    b[15] = (uint8_t)(b[15] << 1);
    if (carry) {
        b[15] ^= 0x87u;
    }
}

void host_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t out[16])
{
    uint8_t k1[16], k2[16], x[16], blk[16];
    uint32_t full, i;

    memset(x, 0, 16u);
    od_test_aes128_encrypt(key, x, k1);
    cmac_dbl(k1);
    memcpy(k2, k1, 16u);
    cmac_dbl(k2);

    full = (len == 0u) ? 0u : ((len - 1u) / 16u);
    memset(x, 0, 16u);
    for (i = 0; i < full; ++i) {
        unsigned j;
        for (j = 0; j < 16u; ++j) { blk[j] = (uint8_t)(x[j] ^ msg[i * 16u + j]); }
        od_test_aes128_encrypt(key, blk, x);
    }
    {
        uint32_t rem = len - (full * 16u);
        unsigned j;
        if (len != 0u && rem == 16u) {
            for (j = 0; j < 16u; ++j) { blk[j] = (uint8_t)(msg[full * 16u + j] ^ k1[j]); }
        } else {
            memset(blk, 0, 16u);
            if (rem > 0u) { memcpy(blk, &msg[full * 16u], rem); }
            blk[rem] = 0x80u;
            for (j = 0; j < 16u; ++j) { blk[j] ^= k2[j]; }
        }
        for (j = 0; j < 16u; ++j) { blk[j] ^= x[j]; }
        od_test_aes128_encrypt(key, blk, out);
    }
}

/* ------------------------------------------------------------------- CCM (RFC 3610) on the host --- */

#define CCM_M 12u
#define CCM_L 2u

static void cbc_absorb(const uint8_t key[16], uint8_t x[16], const uint8_t *p, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i += 16u) {
        unsigned j;
        for (j = 0; j < 16u && i + j < n; ++j) { x[j] ^= p[i + j]; }
        od_test_aes128_encrypt(key, x, x);
    }
}

static void ccm_mac(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                    uint8_t aad_len, const uint8_t *plain, uint16_t len, uint8_t tag[16])
{
    uint8_t x[16];
    uint8_t hdr[2u + 255u];

    x[0] = (uint8_t)((aad_len ? 0x40u : 0u) | (((CCM_M - 2u) / 2u) << 3) | (CCM_L - 1u));
    memcpy(&x[1], nonce, 13u);
    x[14] = (uint8_t)(len >> 8);
    x[15] = (uint8_t)len;
    od_test_aes128_encrypt(key, x, x);
    if (aad_len != 0u) {
        hdr[0] = 0u;
        hdr[1] = aad_len;
        memcpy(&hdr[2], aad, aad_len);
        cbc_absorb(key, x, hdr, 2u + (uint32_t)aad_len);
    }
    if (len != 0u) {
        cbc_absorb(key, x, plain, len);
    }
    memcpy(tag, x, 16u);
}

static void ccm_ctr(const uint8_t key[16], const uint8_t nonce[13], uint16_t i, uint8_t s[16])
{
    uint8_t a[16];

    a[0] = (uint8_t)(CCM_L - 1u);
    memcpy(&a[1], nonce, 13u);
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
    od_test_aes128_encrypt(key, a, s);
}

static void ccm_xor(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *in,
                    uint16_t len, uint8_t *out)
{
    uint8_t s[16];
    uint16_t i;

    for (i = 0; i < len; ++i) {
        if (i % 16u == 0u) {
            ccm_ctr(key, nonce, (uint16_t)(i / 16u + 1u), s);
        }
        out[i] = (uint8_t)(in[i] ^ s[i % 16u]);
    }
}

void host_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                      uint8_t aad_len, const uint8_t *plain, uint16_t len, uint8_t *out)
{
    uint8_t t[16], s0[16];
    unsigned i;

    ccm_mac(key, nonce, aad, aad_len, plain, len, t);
    ccm_xor(key, nonce, plain, len, out);
    ccm_ctr(key, nonce, 0u, s0);
    for (i = 0; i < CCM_M; ++i) { out[len + i] = (uint8_t)(t[i] ^ s0[i]); }
}

bool host_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                      uint8_t aad_len, const uint8_t *in, uint16_t len, uint8_t *plain)
{
    uint8_t t[16], s0[16];
    uint8_t d = 0u;
    uint16_t n;
    unsigned i;

    if (len < CCM_M) { return false; }
    n = (uint16_t)(len - CCM_M);
    ccm_xor(key, nonce, in, n, plain);
    ccm_mac(key, nonce, aad, aad_len, plain, n, t);
    ccm_ctr(key, nonce, 0u, s0);
    for (i = 0; i < CCM_M; ++i) { d |= (uint8_t)(in[n + i] ^ t[i] ^ s0[i]); }
    return d == 0u;
}

/* ----------------------------------------------------------------------------- P-256 (OpenSSL) --- */

void *tp256_new(uint8_t pub[64])
{
    EC_KEY *k = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    uint8_t raw[65];

    if (k == NULL || EC_KEY_generate_key(k) != 1 ||
        EC_POINT_point2oct(EC_KEY_get0_group(k), EC_KEY_get0_public_key(k),
                           POINT_CONVERSION_UNCOMPRESSED, raw, sizeof raw, NULL) != sizeof raw) {
        EC_KEY_free(k);
        return NULL;
    }
    memcpy(pub, &raw[1], 64u);
    return k;
}

bool tp256_agree(void *key, const uint8_t peer[64], uint8_t z[32])
{
    EC_KEY *k = (EC_KEY *)key;
    const EC_GROUP *g;
    EC_POINT *p;
    uint8_t raw[65];
    bool ok;

    if (k == NULL) { return false; }
    g = EC_KEY_get0_group(k);
    p = EC_POINT_new(g);
    raw[0] = 0x04u;
    memcpy(&raw[1], peer, 64u);
    ok = p != NULL && EC_POINT_oct2point(g, p, raw, sizeof raw, NULL) == 1 &&
         ECDH_compute_key(z, 32u, p, k, NULL) == 32;
    EC_POINT_free(p);
    return ok;
}

void tp256_free(void *key)
{
    EC_KEY_free((EC_KEY *)key);
}

/* ------------------------------------------------------------------------------ fake HAL --- */

uint8_t  g_slot_key[OD_HAL_CRYPTO_KEY_SLOTS][16];
static bool g_slot_loaded[OD_HAL_CRYPTO_KEY_SLOTS];
unsigned g_key_set_calls;
unsigned g_key_clear_calls;
uint8_t  g_rand_next;
enum od_hal_crypto_status g_force_status;
int32_t  g_force_after;
unsigned g_hal_calls;
bool     g_ecdh_live;
unsigned g_ecdh_keygens;
static void *g_ecdh_key;

struct fake_secret g_secret[3];
unsigned g_secret_writes;
int32_t  g_secret_fail_after = -1;

bool g_force_once;

static enum od_hal_crypto_status forced(void)
{
    const unsigned n = g_hal_calls++;

    enum od_hal_crypto_status st;

    if (g_force_status == OD_HAL_CRYPTO_OK || g_force_after < 0) { return OD_HAL_CRYPTO_OK; }
    if (n < (unsigned)g_force_after) { return OD_HAL_CRYPTO_OK; }
    st = g_force_status;
    if (g_force_once) { g_force_status = OD_HAL_CRYPTO_OK; }
    return st;
}

bool fake_slot_loaded(unsigned slot)
{
    return slot < OD_HAL_CRYPTO_KEY_SLOTS && g_slot_loaded[slot];
}

void fake_reset(void)
{
    memset(g_slot_key, 0, sizeof g_slot_key);
    memset(g_slot_loaded, 0, sizeof g_slot_loaded);
    g_key_set_calls = 0;
    g_key_clear_calls = 0;
    g_rand_next = 0;
    g_force_status = OD_HAL_CRYPTO_OK;
    g_force_once = false;
    g_force_after = 0;
    g_hal_calls = 0;
    tp256_free(g_ecdh_key);
    g_ecdh_key = NULL;
    g_ecdh_live = false;
    g_ecdh_keygens = 0;
    memset(g_secret, 0, sizeof g_secret);
    g_secret_writes = 0;
    g_secret_fail_after = -1;
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot, const uint8_t key[16])
{
    const enum od_hal_crypto_status f = forced();

    if (f != OD_HAL_CRYPTO_OK) { return f; }
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS) { return OD_HAL_CRYPTO_ERROR; }
    ++g_key_set_calls;
    memcpy(g_slot_key[slot], key, 16u);
    g_slot_loaded[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS) { return; }
    ++g_key_clear_calls;
    memset(g_slot_key[slot], 0, 16u);
    g_slot_loaded[slot] = false;
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    const enum od_hal_crypto_status f = forced();

    if (f != OD_HAL_CRYPTO_OK) { return f; }
    host_cmac(key, msg, msg_len, out);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    const enum od_hal_crypto_status f = forced();

    if (f != OD_HAL_CRYPTO_OK) { return f; }
    od_test_aes128_encrypt(key, in, out);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    const enum od_hal_crypto_status f = forced();
    uint16_t i;

    if (f != OD_HAL_CRYPTO_OK) { return f; }
    for (i = 0; i < len; ++i) { buf[i] = g_rand_next++; }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len)
{
    const enum od_hal_crypto_status f = forced();

    if (f != OD_HAL_CRYPTO_OK) { return f; }
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || !g_slot_loaded[slot] || nonce_len != 13u ||
        plain == NULL || ct == NULL || ct_len == NULL ||
        ct_cap < (uint16_t)(plain_len + OD_HAL_CRYPTO_TAG_LEN)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    host_ccm_encrypt(g_slot_key[slot], nonce, aad, aad_len, plain, plain_len, ct);
    *ct_len = (uint16_t)(plain_len + OD_HAL_CRYPTO_TAG_LEN);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len)
{
    const enum od_hal_crypto_status f = forced();
    uint8_t tmp[512];
    uint16_t body;

    if (f != OD_HAL_CRYPTO_OK) { return f; }
    /* The HAL contract: plain is never NULL, even for an empty plaintext (od_hal_crypto.h). */
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || !g_slot_loaded[slot] || nonce_len != 13u ||
        plain == NULL || plain_len == NULL ||
        ct_len < OD_HAL_CRYPTO_TAG_LEN || ct_len - OD_HAL_CRYPTO_TAG_LEN > sizeof tmp) {
        return OD_HAL_CRYPTO_ERROR;
    }
    body = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    if (plain_cap < body) { return OD_HAL_CRYPTO_ERROR; }
    if (!host_ccm_decrypt(g_slot_key[slot], nonce, aad, aad_len, ct, ct_len, tmp)) {
        return OD_HAL_CRYPTO_AUTH_FAILED;
    }
    if (body != 0u) { memcpy(plain, tmp, body); }
    *plain_len = body;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_ecdh_discard(void)
{
    tp256_free(g_ecdh_key);
    g_ecdh_key = NULL;
    g_ecdh_live = false;
}

enum od_hal_crypto_status od_hal_crypto_ecdh_keygen(uint8_t pub[64])
{
    const enum od_hal_crypto_status f = forced();

    od_hal_crypto_ecdh_discard();
    if (f != OD_HAL_CRYPTO_OK) { return f; }
    g_ecdh_key = tp256_new(pub);
    if (g_ecdh_key == NULL) { return OD_HAL_CRYPTO_ERROR; }
    g_ecdh_live = true;
    ++g_ecdh_keygens;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ecdh_agree(const uint8_t peer[64], uint8_t z[32])
{
    const enum od_hal_crypto_status f = forced();
    bool ok;

    if (f != OD_HAL_CRYPTO_OK || !g_ecdh_live) {
        od_hal_crypto_ecdh_discard();
        return (f != OD_HAL_CRYPTO_OK) ? f : OD_HAL_CRYPTO_ERROR;
    }
    ok = tp256_agree(g_ecdh_key, peer, z);
    od_hal_crypto_ecdh_discard();
    return ok ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_BAD_POINT;
}

/* ------------------------------------------------------------------------- fake secret store --- */

bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *buf, uint16_t len)
{
    if ((unsigned)id > 2u || !g_secret[id].present || g_secret[id].len != len) { return false; }
    memcpy(buf, g_secret[id].data, len);
    return true;
}

bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len)
{
    if ((unsigned)id > 2u || len > sizeof g_secret[id].data) { return false; }
    if (g_secret_fail_after == 0) { return false; }
    if (g_secret_fail_after > 0) { --g_secret_fail_after; }
    memcpy(g_secret[id].data, buf, len);
    g_secret[id].len = len;
    g_secret[id].present = true;
    ++g_secret_writes;
    return true;
}

bool od_hal_secret_erase(enum od_hal_secret_id id)
{
    if ((unsigned)id > 2u) { return false; }
    memset(&g_secret[id], 0, sizeof g_secret[id]);
    return true;
}

/* --------------------------------------------------------------------------------- fixture --- */

const uint8_t MASTER[16] = {
    0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
};
const uint8_t UID[8] = { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF };

struct SecurityConfig g_sec;

void sec_init(uint16_t timeout_s)
{
    memset(&g_sec, 0, sizeof g_sec);
    g_sec.encryption_enabled = 1u;
    memcpy(g_sec.encryption_key, MASTER, 16u);
    g_sec.session_timeout_seconds = timeout_s;
}
