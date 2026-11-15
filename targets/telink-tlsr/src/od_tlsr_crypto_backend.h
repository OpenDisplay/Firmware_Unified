/* The three primitives od_tlsr_crypto.c builds od_hal_crypto from. The chip implements them
 * (od_tlsr_crypto_hw.c: the AES block, the SDK random generator, the SDK's software P-256);
 * tests/host/tlsr_crypto_test.c implements them over reference code. Byte order here is the
 * HAL's: AES as FIPS-197, P-256 coordinates and the shared secret big-endian. */
#ifndef OD_TLSR_CRYPTO_BACKEND_H
#define OD_TLSR_CRYPTO_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

bool od_tlsr_aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
bool od_tlsr_random(uint8_t *buf, uint16_t len);
bool od_tlsr_p256_keygen(uint8_t pub[64], uint8_t priv[32]);
/* The peer point has been checked on the curve by the caller. */
bool od_tlsr_p256_agree(const uint8_t peer[64], const uint8_t priv[32], uint8_t z[32]);

#endif /* OD_TLSR_CRYPTO_BACKEND_H */
