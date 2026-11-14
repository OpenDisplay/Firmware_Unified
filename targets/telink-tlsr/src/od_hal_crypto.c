/* Shared crypto HAL over the TLSR825x AES-128 engine and od_aes_modes.c.
 *
 * THE ENGINE'S BYTE ORDER IS MEASURED, NOT ASSUMED. Telink's BLE stack feeds this engine
 * byte-reversed operands for the link layer, and the driver's own convention is undocumented.
 * The first use runs the FIPS-197 Appendix C.1 known-answer test both ways round and keeps the
 * orientation that matches; if neither does, every call fails, so a session can never be built
 * on a miscomputed block. */

#include "od_hal_crypto.h"

#include "od_aes_modes.h"
#include "tlsr_port.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

enum engine_mode { ENGINE_UNTESTED = 0, ENGINE_DIRECT, ENGINE_REVERSED, ENGINE_BROKEN };

static uint8_t s_mode = ENGINE_UNTESTED;
static uint8_t s_keys[OD_HAL_CRYPTO_KEY_SLOTS][16];
static bool    s_key_ready[OD_HAL_CRYPTO_KEY_SLOTS];

static void reverse16(uint8_t *dst, const uint8_t *src)
{
    uint8_t i;
    for (i = 0; i < 16u; ++i) {
        dst[i] = src[15u - i];
    }
}

static void engine_block(uint8_t mode, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t rk[16], ri[16], ro[16];

    if (mode == ENGINE_DIRECT) {
        tlsr_port_aes_block(key, in, out);
        return;
    }
    reverse16(rk, key);
    reverse16(ri, in);
    tlsr_port_aes_block(rk, ri, ro);
    reverse16(out, ro);
    memset(rk, 0, sizeof(rk));
}

static bool engine_ready(void)
{
    static const uint8_t k[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                   0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    static const uint8_t p[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                   0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t c[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                   0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    uint8_t out[16];

    if (s_mode == ENGINE_UNTESTED) {
        engine_block(ENGINE_DIRECT, k, p, out);
        if (memcmp(out, c, 16) == 0) {
            s_mode = ENGINE_DIRECT;
        } else {
            engine_block(ENGINE_REVERSED, k, p, out);
            s_mode = memcmp(out, c, 16) == 0 ? ENGINE_REVERSED : ENGINE_BROKEN;
        }
    }
    return s_mode == ENGINE_DIRECT || s_mode == ENGINE_REVERSED;
}

static bool aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (!engine_ready()) {
        return false;
    }
    engine_block(s_mode, key, in, out);
    return true;
}

/* ------------------------------------------------------------------------ key slots --- */

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot, const uint8_t key[16])
{
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || key == NULL || !engine_ready()) {
        return OD_HAL_CRYPTO_ERROR;
    }
    memcpy(s_keys[slot], key, 16u);
    s_key_ready[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    if (slot < OD_HAL_CRYPTO_KEY_SLOTS) {
        memset(s_keys[slot], 0, 16u);
        s_key_ready[slot] = false;
    }
}

/* ------------------------------------------------------------------------------ CCM --- */

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len, uint8_t *ct, uint16_t ct_cap,
        uint16_t *ct_len)
{
    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || !s_key_ready[slot] || ct == NULL || ct_len == NULL ||
        (uint32_t)plain_len + OD_HAL_CRYPTO_TAG_LEN > ct_cap) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (!od_aes_ccm_encrypt(aes_block, s_keys[slot], nonce, nonce_len, aad, aad_len,
                            plain, plain_len, OD_HAL_CRYPTO_TAG_LEN, ct)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    *ct_len = (uint16_t)(plain_len + OD_HAL_CRYPTO_TAG_LEN);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len, uint8_t *plain, uint16_t plain_cap,
        uint16_t *plain_len)
{
    bool auth_ok = false;

    if (slot >= OD_HAL_CRYPTO_KEY_SLOTS || !s_key_ready[slot] || ct == NULL ||
        ct_len <= OD_HAL_CRYPTO_TAG_LEN || plain == NULL || plain_len == NULL ||
        plain_cap < (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (!od_aes_ccm_decrypt(aes_block, s_keys[slot], nonce, nonce_len, aad, aad_len,
                            ct, ct_len, OD_HAL_CRYPTO_TAG_LEN, plain, &auth_ok)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (!auth_ok) {
        return OD_HAL_CRYPTO_AUTH_FAILED;
    }
    *plain_len = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    return OD_HAL_CRYPTO_OK;
}

/* -------------------------------------------------------------------- CMAC and ECB --- */

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    return od_aes_cmac(aes_block, key, msg, msg_len, out) ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_ERROR;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    if (key == NULL || in == NULL || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    return aes_block(key, in, out) ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_ERROR;
}

/* --------------------------------------------------------------------------- random --- */

/* The SDK's rand() is an LFSR seeded from analog noise: fine as entropy input, not as output.
 * So it seeds an AES-CTR generator: K = CMAC_0(noise || ticks || MAC), output E_K(V++), and K
 * is replaced by the next block after every request so earlier output cannot be recomputed from
 * a later state. The quality of the noise source itself is unmeasured on this silicon. */
static uint8_t s_drbg_key[16];
static uint8_t s_drbg_v[16];
static bool    s_drbg_seeded;

static void ctr_inc(uint8_t v[16])
{
    uint8_t i = 16u;
    while (i-- > 0u && ++v[i] == 0u) {
    }
}

static bool drbg_seed(void)
{
    static const uint8_t zero_key[16];
    uint8_t seed[16 * 4 + 4 + 6];
    uint32_t w, t;
    uint8_t i;

    for (i = 0; i < 16u; ++i) {
        w = tlsr_port_noise32();
        memcpy(&seed[i * 4u], &w, 4u);
    }
    t = tlsr_port_ticks();
    memcpy(&seed[64], &t, 4u);
    tlsr_port_mac(&seed[68]);
    if (!od_aes_cmac(aes_block, zero_key, seed, sizeof(seed), s_drbg_key)) {
        return false;
    }
    seed[0] ^= 0x5Au;                       /* domain-separate V from K */
    if (!od_aes_cmac(aes_block, s_drbg_key, seed, sizeof(seed), s_drbg_v)) {
        return false;
    }
    memset(seed, 0, sizeof(seed));
    s_drbg_seeded = true;
    return true;
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    uint8_t block[16];
    uint16_t off = 0u;
    uint8_t n;

    if (buf == NULL && len != 0u) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (!s_drbg_seeded && !drbg_seed()) {
        return OD_HAL_CRYPTO_ERROR;
    }
    while (off < len) {
        ctr_inc(s_drbg_v);
        if (!aes_block(s_drbg_key, s_drbg_v, block)) {
            return OD_HAL_CRYPTO_ERROR;
        }
        n = (uint8_t)((len - off) < 16u ? (len - off) : 16u);
        memcpy(buf + off, block, n);
        off = (uint16_t)(off + n);
    }
    ctr_inc(s_drbg_v);
    if (!aes_block(s_drbg_key, s_drbg_v, s_drbg_key)) {  /* rekey: backtracking resistance */
        return OD_HAL_CRYPTO_ERROR;
    }
    memset(block, 0, sizeof(block));
    return OD_HAL_CRYPTO_OK;
}
