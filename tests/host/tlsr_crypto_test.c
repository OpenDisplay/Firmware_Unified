/* tlsr_crypto_test.c -- the Telink od_hal_crypto (targets/telink-tlsr/src/od_tlsr_crypto.c),
 * its software CCM and CMAC over one AES block primitive, checked against independent
 * references: RFC 4493's CMAC vectors, OpenSSL's AES-128-CCM, and OpenSSL's ECDH.
 *
 * The backend (fake_tlsr/tlsr_crypto_ref.c) stands in for the chip's AES engine and software
 * P-256, so what this proves is the mode code and the key lifecycle, not the chip's byte order
 * -- the firmware detects that at boot (od_tlsr_crypto_hw.c). */
#define OPENSSL_SUPPRESS_DEPRECATED 1

#include "od_hal_crypto.h"

#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>

#include <stdio.h>
#include <string.h>

extern bool tlsr_ref_fail_random;
extern unsigned tlsr_ref_agrees;

static unsigned g_checks, g_failures;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            ++g_failures;                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                  \
        }                                                                       \
    } while (0)

static void hex(uint8_t *out, const char *s)
{
    unsigned v;
    while (*s != '\0') {
        (void)sscanf(s, "%2x", &v);
        *out++ = (uint8_t)v;
        s += 2;
    }
}

static void cmac_vectors(void)
{
    static const char *const msg_hex =
        "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
        "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710";
    static const struct {
        uint32_t len;
        const char *mac;
    } v[] = {
        {0u, "bb1d6929e95937287fa37d129b756746"},
        {16u, "070a16b46b4d4144f79bdd9dd04a287c"},
        {40u, "dfa66747de9ae63030ca32611497c827"},
        {64u, "51f0bebf7e3b9d92fc49741779363cfe"},
    };
    uint8_t key[16], msg[64], want[16], got[16];
    unsigned i;

    hex(key, "2b7e151628aed2a6abf7158809cf4f3c");
    hex(msg, msg_hex);
    for (i = 0; i < sizeof v / sizeof v[0]; ++i) {
        hex(want, v[i].mac);
        CHECK(od_hal_crypto_cmac(key, msg, v[i].len, got) == OD_HAL_CRYPTO_OK);
        CHECK(memcmp(got, want, 16) == 0);
    }
    CHECK(od_hal_crypto_cmac(key, NULL, 0u, got) == OD_HAL_CRYPTO_OK);
    hex(want, v[0].mac);
    CHECK(memcmp(got, want, 16) == 0);
    CHECK(od_hal_crypto_cmac(key, NULL, 1u, got) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_cmac(NULL, msg, 1u, got) == OD_HAL_CRYPTO_ERROR);
    /* FIPS-197 C.1 through the ECB entry point */
    {
        uint8_t k[16], in[16], out[16], exp[16];
        hex(k, "000102030405060708090a0b0c0d0e0f");
        hex(in, "00112233445566778899aabbccddeeff");
        hex(exp, "69c4e0d86a7b0430d8cdb78070b4c55a");
        CHECK(od_hal_crypto_aes_ecb(k, in, out) == OD_HAL_CRYPTO_OK && memcmp(out, exp, 16) == 0);
        CHECK(od_hal_crypto_aes_ecb(k, NULL, out) == OD_HAL_CRYPTO_ERROR);
    }
}

/* OpenSSL AES-128-CCM, 13-byte nonce, 12-byte tag: out = C || T. */
static void ref_ccm(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad, int aad_len,
                    const uint8_t *pt, int len, uint8_t *out)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int n;

    EVP_EncryptInit_ex(c, EVP_aes_128_ccm(), NULL, NULL, NULL);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_IVLEN, 13, NULL);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 12, NULL);
    EVP_EncryptInit_ex(c, NULL, NULL, key, nonce);
    EVP_EncryptUpdate(c, NULL, &n, NULL, len);
    if (aad_len > 0) {
        EVP_EncryptUpdate(c, NULL, &n, aad, aad_len);
    }
    EVP_EncryptUpdate(c, out, &n, pt, len);
    EVP_EncryptFinal_ex(c, out + len, &n);
    EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 12, out + len);
    EVP_CIPHER_CTX_free(c);
}

static void ccm(void)
{
    static uint8_t pt[600], ct[600 + 12], want[600 + 12], back[600];
    uint8_t key[16], nonce[13], aad[40];
    uint16_t lens[] = {0u, 1u, 15u, 16u, 17u, 31u, 32u, 200u, 244u, 600u};
    uint8_t aads[] = {0u, 1u, 7u, 14u, 16u, 40u};
    unsigned i, j, k;
    uint16_t n;

    for (k = 0; k < sizeof pt; ++k) {
        pt[k] = (uint8_t)(k * 29u + 3u);
    }
    for (k = 0; k < sizeof aad; ++k) {
        aad[k] = (uint8_t)(k ^ 0x5Au);
    }
    hex(key, "c0c1c2c3c4c5c6c7c8c9cacbcccdcecf");
    hex(nonce, "00000003020100a0a1a2a3a4a5");
    CHECK(od_hal_crypto_key_set(1u, key) == OD_HAL_CRYPTO_OK);
    for (i = 0; i < sizeof lens / sizeof lens[0]; ++i) {
        for (j = 0; j < sizeof aads; ++j) {
            ref_ccm(key, nonce, aad, aads[j], pt, lens[i], want);
            n = 0u;
            CHECK(od_hal_crypto_ccm_encrypt(1u, nonce, 13u, aad, aads[j], pt, lens[i], ct,
                                            (uint16_t)sizeof ct, &n) == OD_HAL_CRYPTO_OK);
            CHECK(n == lens[i] + 12u && memcmp(ct, want, n) == 0);
            memset(back, 0xEE, sizeof back);
            CHECK(od_hal_crypto_ccm_decrypt(1u, nonce, 13u, aad, aads[j], ct, n, back,
                                            (uint16_t)sizeof back, &n) == OD_HAL_CRYPTO_OK);
            CHECK(n == lens[i] && memcmp(back, pt, n) == 0);
        }
    }
    /* any flipped bit, in C, in T or in the AAD, fails and leaves no plaintext */
    ref_ccm(key, nonce, aad, 16, pt, 50, ct);
    for (k = 0; k < 62u; k += 7u) {
        ct[k] ^= 0x10u;
        memset(back, 0xEE, sizeof back);
        CHECK(od_hal_crypto_ccm_decrypt(1u, nonce, 13u, aad, 16u, ct, 62u, back, sizeof back, &n) ==
              OD_HAL_CRYPTO_AUTH_FAILED);
        for (j = 0; j < 50u; ++j) {
            if (back[j] == pt[j] && pt[j] != 0u) {
                break;
            }
        }
        CHECK(j == 50u);
        ct[k] ^= 0x10u;
    }
    aad[3] ^= 1u;
    CHECK(od_hal_crypto_ccm_decrypt(1u, nonce, 13u, aad, 16u, ct, 62u, back, sizeof back, &n) ==
          OD_HAL_CRYPTO_AUTH_FAILED);
    aad[3] ^= 1u;
    /* the slot's own key, not another slot's */
    {
        uint8_t other[16];
        memcpy(other, key, 16);
        other[0] ^= 1u;
        CHECK(od_hal_crypto_key_set(0u, other) == OD_HAL_CRYPTO_OK);
    }
    CHECK(od_hal_crypto_ccm_decrypt(0u, nonce, 13u, aad, 16u, ct, 62u, back, sizeof back, &n) ==
          OD_HAL_CRYPTO_AUTH_FAILED);
    /* argument contract */
    CHECK(od_hal_crypto_ccm_decrypt(1u, nonce, 13u, aad, 16u, ct, 62u, back, 49u, &n) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ccm_decrypt(1u, nonce, 13u, aad, 16u, ct, 11u, back, sizeof back, &n) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ccm_encrypt(1u, nonce, 13u, aad, 16u, pt, 50u, ct, 61u, &n) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ccm_encrypt(2u, nonce, 13u, aad, 16u, pt, 50u, ct, sizeof ct, &n) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ccm_encrypt(1u, nonce, 13u, NULL, 16u, pt, 50u, ct, sizeof ct, &n) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ccm_encrypt(1u, nonce, 6u, aad, 16u, pt, 50u, ct, sizeof ct, &n) == OD_HAL_CRYPTO_ERROR);
    od_hal_crypto_key_clear(1u);
    CHECK(od_hal_crypto_ccm_encrypt(1u, nonce, 13u, aad, 16u, pt, 50u, ct, sizeof ct, &n) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_key_set(5u, key) == OD_HAL_CRYPTO_ERROR);
}

static void ecdh(void)
{
    EC_KEY *peer = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT *p;
    uint8_t mine[64], theirs[64], raw[65], z[32], zref[32];

    CHECK(peer != NULL && EC_KEY_generate_key(peer) == 1);
    CHECK(EC_POINT_point2oct(EC_KEY_get0_group(peer), EC_KEY_get0_public_key(peer),
                             POINT_CONVERSION_UNCOMPRESSED, raw, sizeof raw, NULL) == sizeof raw);
    memcpy(theirs, &raw[1], 64u);

    CHECK(od_hal_crypto_ecdh_keygen(mine) == OD_HAL_CRYPTO_OK);
    CHECK(od_hal_crypto_ecdh_agree(theirs, z) == OD_HAL_CRYPTO_OK);
    p = EC_POINT_new(EC_KEY_get0_group(peer));
    raw[0] = 0x04u;
    memcpy(&raw[1], mine, 64u);
    CHECK(p != NULL && EC_POINT_oct2point(EC_KEY_get0_group(peer), p, raw, sizeof raw, NULL) == 1);
    CHECK(ECDH_compute_key(zref, 32u, p, peer, NULL) == 32);
    CHECK(memcmp(z, zref, 32) == 0);
    /* one agreement per key pair */
    CHECK(od_hal_crypto_ecdh_agree(theirs, z) == OD_HAL_CRYPTO_ERROR);
    /* an off-curve point is refused before the backend sees it */
    CHECK(od_hal_crypto_ecdh_keygen(mine) == OD_HAL_CRYPTO_OK);
    theirs[63] ^= 1u;
    tlsr_ref_agrees = 0u;
    CHECK(od_hal_crypto_ecdh_agree(theirs, z) == OD_HAL_CRYPTO_BAD_POINT);
    CHECK(tlsr_ref_agrees == 0u);
    theirs[63] ^= 1u;
    CHECK(od_hal_crypto_ecdh_agree(theirs, z) == OD_HAL_CRYPTO_ERROR); /* consumed by the refusal */
    /* discard drops the pair */
    CHECK(od_hal_crypto_ecdh_keygen(mine) == OD_HAL_CRYPTO_OK);
    od_hal_crypto_ecdh_discard();
    CHECK(od_hal_crypto_ecdh_agree(theirs, z) == OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ecdh_keygen(NULL) == OD_HAL_CRYPTO_ERROR);
    EC_POINT_free(p);
    EC_KEY_free(peer);
}

static void random_contract(void)
{
    uint8_t a[32], b[32];

    CHECK(od_hal_crypto_random(a, sizeof a) == OD_HAL_CRYPTO_OK);
    CHECK(od_hal_crypto_random(b, sizeof b) == OD_HAL_CRYPTO_OK);
    CHECK(memcmp(a, b, sizeof a) != 0);
    CHECK(od_hal_crypto_random(NULL, 0u) == OD_HAL_CRYPTO_OK);
    CHECK(od_hal_crypto_random(NULL, 1u) == OD_HAL_CRYPTO_ERROR);
    tlsr_ref_fail_random = true;
    CHECK(od_hal_crypto_random(a, sizeof a) == OD_HAL_CRYPTO_ERROR);
    tlsr_ref_fail_random = false;
}

int main(void)
{
    cmac_vectors();
    ccm();
    ecdh();
    random_contract();
    printf("tlsr_crypto: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
