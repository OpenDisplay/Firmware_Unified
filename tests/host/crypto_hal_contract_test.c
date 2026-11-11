/* crypto_hal_contract_test.c -- the od_hal_crypto.h buffer contract, against the PRODUCTION
 * PSA HAL files (Nordic and BG22, one executable each) over tests/host/fake_psa/, and against
 * the session fake every SEC2 host suite links.
 *
 * The contract: plain is never NULL, on encrypt or decrypt, even for an empty plaintext; NULL
 * is ERROR on every target, refused before the backend. BG22's PSA refused a NULL decrypt
 * buffer on hardware while the HAL called it legal, so a NULL that one backend accepts and
 * another refuses is exactly the divergence this pins. The fake PSA refuses NULL as BG22 does.
 *
 * What this cannot prove: that real PSA accepts a NON-NULL zero-length buffer. That is the
 * hardware re-check (BG22 canary: CCM decrypt of a tag-only record with plain_cap 0). The ESP32
 * mbedTLS HAL is not host-compiled; its guard is the same expression. */
#include "od_hal_crypto.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks, g_failures;
#define CHECK(c)                                                                   \
    do {                                                                           \
        ++g_checks;                                                                \
        if (!(c)) {                                                                \
            ++g_failures;                                                          \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                     \
        }                                                                          \
    } while (0)

#ifdef OD_CONTRACT_STUB_LOG
/* The Nordic HAL logs through od_log; nothing here needs the output. */
void _od_log(int level, const char *fmt, ...);
void _od_log(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
#endif

static const uint8_t KEY[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

int main(void)
{
    uint8_t nonce[13] = { 0 };
    uint8_t aad[3] = { 0x00u, 0x70u, 0x01u };
    uint8_t plain[8] = { 0x55u };
    uint8_t one[1];
    uint8_t ct[32];
    uint16_t ct_len = 0u, plain_len = 0xFFFFu;

    CHECK(od_hal_crypto_key_set(0, KEY) == OD_HAL_CRYPTO_OK);

    /* encrypt: an empty plaintext needs a real buffer */
    CHECK(od_hal_crypto_ccm_encrypt(0, nonce, 13u, aad, 3u, NULL, 0u, ct, sizeof ct, &ct_len) ==
          OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ccm_encrypt(0, nonce, 13u, aad, 3u, plain, 0u, ct, sizeof ct, &ct_len) ==
          OD_HAL_CRYPTO_OK);
    CHECK(ct_len == OD_HAL_CRYPTO_TAG_LEN);

    /* decrypt of a tag-only record: NULL is refused, a zero-capacity buffer is not */
    CHECK(od_hal_crypto_ccm_decrypt(0, nonce, 13u, aad, 3u, ct, ct_len, NULL, 0u, &plain_len) ==
          OD_HAL_CRYPTO_ERROR);
    CHECK(plain_len == 0xFFFFu);
    CHECK(od_hal_crypto_ccm_decrypt(0, nonce, 13u, aad, 3u, ct, ct_len, one, 0u, &plain_len) ==
          OD_HAL_CRYPTO_OK);
    CHECK(plain_len == 0u);

    /* and a non-empty one round-trips */
    CHECK(od_hal_crypto_ccm_encrypt(0, nonce, 13u, aad, 3u, plain, 8u, ct, sizeof ct, &ct_len) ==
          OD_HAL_CRYPTO_OK);
    memset(plain, 0, sizeof plain);
    CHECK(od_hal_crypto_ccm_decrypt(0, nonce, 13u, aad, 3u, ct, ct_len, NULL, 0u, &plain_len) ==
          OD_HAL_CRYPTO_ERROR);
    CHECK(od_hal_crypto_ccm_decrypt(0, nonce, 13u, aad, 3u, ct, ct_len, plain, 8u, &plain_len) ==
          OD_HAL_CRYPTO_OK);
    CHECK(plain_len == 8u && plain[0] == 0x55u);

    od_hal_crypto_key_clear(0);
    printf("crypto_hal_contract: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
