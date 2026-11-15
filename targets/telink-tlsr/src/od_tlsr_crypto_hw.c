/* The crypto backend on the chip: the AES-128 engine, the SDK random generator and the SDK's
 * software P-256 (sw_ecc, in the SDK's binary library).
 *
 * BYTE ORDER IS ESTABLISHED, NOT ASSUMED. Neither the AES driver nor sw_ecc documents its byte
 * order, and Telink's link layer uses reversed-order AES elsewhere. So the AES engine is run
 * once against the FIPS-197 C.1 vector in both orders, and a generated P-256 key is accepted in
 * whichever coordinate order lies on the curve. A part on which neither order checks out
 * reports a crypto error rather than producing wrong ciphertext. */
#include "od_tlsr_sdk.h"

#include "od_tlsr_crypto_backend.h"

#include "od_sec2.h"

#include "algorithm/ecc/sw_ecc.h"

#include <string.h>

enum order { ORDER_UNKNOWN = 0, ORDER_AS_IS, ORDER_REVERSED, ORDER_BROKEN };

static enum order s_aes_order;
static enum order s_ecc_order;

static void reverse(uint8_t *dst, const uint8_t *src, unsigned n)
{
    unsigned k;
    for (k = 0u; k < n; ++k) {
        dst[k] = src[n - 1u - k];
    }
}

static void aes_raw(enum order o, const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    uint8_t k[16], d[16], r[16];

    if (o == ORDER_REVERSED) {
        reverse(k, key, 16u);
        reverse(d, in, 16u);
        aes_encrypt(k, d, r);
        reverse(out, r, 16u);
    } else {
        memcpy(k, key, 16u);
        memcpy(d, in, 16u);
        aes_encrypt(k, d, out);
    }
    memset(k, 0, sizeof k);
}

static void aes_detect(void)
{
    /* FIPS-197 appendix C.1 */
    static const uint8_t key[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
    static const uint8_t pt[16] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
    static const uint8_t ct[16] = { 0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                    0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
    uint8_t out[16];

    aes_raw(ORDER_AS_IS, key, pt, out);
    if (memcmp(out, ct, 16u) == 0) {
        s_aes_order = ORDER_AS_IS;
        return;
    }
    aes_raw(ORDER_REVERSED, key, pt, out);
    s_aes_order = memcmp(out, ct, 16u) == 0 ? ORDER_REVERSED : ORDER_BROKEN;
}

bool od_tlsr_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (s_aes_order == ORDER_UNKNOWN) {
        aes_detect();
    }
    if (s_aes_order == ORDER_BROKEN) {
        return false;
    }
    aes_raw(s_aes_order, key, in, out);
    return true;
}

/* generateRandomNum is the SDK's generator, seeded by random_generator_init() at boot from
 * analog noise (main.c). Its entropy on this part is not characterised: REPORT.md records it as
 * an open item for SEC2. */
bool od_tlsr_random(uint8_t *buf, uint16_t len)
{
    generateRandomNum((int)len, buf);
    return true;
}

static int ecc_rng(unsigned char *dest, unsigned size)
{
    generateRandomNum((int)size, dest);
    return 1;
}

static void coords_swap(uint8_t dst[64], const uint8_t src[64])
{
    reverse(dst, src, 32u);
    reverse(dst + 32, src + 32, 32u);
}

bool od_tlsr_p256_keygen(uint8_t pub[64], uint8_t priv[32])
{
    uint8_t raw[64];

    swECC_set_rng(ecc_rng);
    if (swECC_make_key(raw, priv, ECC_use_secp256r1) != 1u) {
        return false;
    }
    if (s_ecc_order != ORDER_REVERSED && od_sec2_point_valid(raw)) {
        s_ecc_order = ORDER_AS_IS;
        memcpy(pub, raw, 64u);
        return true;
    }
    coords_swap(pub, raw);
    if (od_sec2_point_valid(pub)) {
        s_ecc_order = ORDER_REVERSED;
        return true;
    }
    s_ecc_order = ORDER_BROKEN;
    return false;
}

bool od_tlsr_p256_agree(const uint8_t peer[64], const uint8_t priv[32], uint8_t z[32])
{
    uint8_t raw[64], secret[32];

    if (s_ecc_order != ORDER_AS_IS && s_ecc_order != ORDER_REVERSED) {
        return false;
    }
    if (s_ecc_order == ORDER_REVERSED) {
        coords_swap(raw, peer);
    } else {
        memcpy(raw, peer, 64u);
    }
    if (swECC_shared_secret(raw, priv, secret, ECC_use_secp256r1) != 1u) {
        return false;
    }
    if (s_ecc_order == ORDER_REVERSED) {
        reverse(z, secret, 32u);
    } else {
        memcpy(z, secret, 32u);
    }
    memset(secret, 0, sizeof secret);
    return true;
}
