/* The three od_tlsr_crypto_backend.h primitives over host reference code, so the production
 * od_tlsr_crypto.c (CCM, CMAC, the ECDH key lifecycle) runs on the host: AES from aes128.c,
 * P-256 from OpenSSL with the scalar crossing the boundary as 32 big-endian bytes exactly as
 * the chip's software ECC hands it over. */
#define OPENSSL_SUPPRESS_DEPRECATED 1

#include "od_tlsr_crypto_backend.h"

#include "aes128.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>

#include <string.h>

bool tlsr_ref_fail_random;
unsigned tlsr_ref_agrees;

bool od_tlsr_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    od_test_aes128_encrypt(key, in, out);
    return true;
}

bool od_tlsr_random(uint8_t *buf, uint16_t len)
{
    if (tlsr_ref_fail_random) {
        return false;
    }
    return len == 0u || RAND_bytes(buf, len) == 1;
}

bool od_tlsr_p256_keygen(uint8_t pub[64], uint8_t priv[32])
{
    EC_KEY *k = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    uint8_t raw[65];
    bool ok;

    ok = k != NULL && EC_KEY_generate_key(k) == 1 &&
         EC_POINT_point2oct(EC_KEY_get0_group(k), EC_KEY_get0_public_key(k),
                            POINT_CONVERSION_UNCOMPRESSED, raw, sizeof raw, NULL) == sizeof raw &&
         BN_bn2binpad(EC_KEY_get0_private_key(k), priv, 32) == 32;
    if (ok) {
        memcpy(pub, &raw[1], 64u);
    }
    EC_KEY_free(k);
    return ok;
}

bool od_tlsr_p256_agree(const uint8_t peer[64], const uint8_t priv[32], uint8_t z[32])
{
    EC_KEY *k = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    BIGNUM *d = BN_bin2bn(priv, 32, NULL);
    EC_POINT *p = NULL;
    uint8_t raw[65];
    bool ok = false;

    tlsr_ref_agrees++;
    if (k != NULL && d != NULL && EC_KEY_set_private_key(k, d) == 1) {
        p = EC_POINT_new(EC_KEY_get0_group(k));
        raw[0] = 0x04u;
        memcpy(&raw[1], peer, 64u);
        ok = p != NULL && EC_POINT_oct2point(EC_KEY_get0_group(k), p, raw, sizeof raw, NULL) == 1 &&
             ECDH_compute_key(z, 32u, p, k, NULL) == 32;
    }
    EC_POINT_free(p);
    BN_free(d);
    EC_KEY_free(k);
    return ok;
}
