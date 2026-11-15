/* od_sec2_kdf.h -- THE SEC2 KEY-DERIVATION CONSTRUCTION, behind one interface.
 *
 * SECURITY_V2 section 2.1. Two functions, and nothing else in the firmware knows how a key is
 * derived: the state machines call these with a label and a context and receive 16 bytes.
 *
 * AWAITING A CRYPTOGRAPHER (section 19 E1, E2): the extractor assumption and the SP 800-108r1 K0
 * question are open. If the construction changes, it changes HERE ONLY -- od_sec2.c is written
 * against these two signatures and must not grow its own CMAC chain.
 *
 *   extract  PRK = CMAC(salt, Z)
 *   kdf      CMAC(K_in, 0x01 || Label || 0x00 || Context || 0x00 0x80)    one block, L = 128
 */
#ifndef OD_SEC2_KDF_H
#define OD_SEC2_KDF_H

#include "od_hal_crypto.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest Label (section 2.3): "OD2/v1-compat", 13 bytes. Longest Context: Tc, 179 bytes. */
#define OD_SEC2_LABEL_MAX   13u
#define OD_SEC2_CTX_MAX     179u

/* PRK = CMAC(salt, Z), Z the 32-byte ECDH x-coordinate. salt NULL means 0^16 (the claim). */
enum od_hal_crypto_status od_sec2_extract(const uint8_t salt[16], const uint8_t z[32],
                                          uint8_t prk[16]);

/* out = KDF(k_in, label, ctx). label is a NUL-terminated ASCII string from the section 2.3
 * registry; ctx_len must be <= OD_SEC2_CTX_MAX, else ERROR. out may alias k_in. */
enum od_hal_crypto_status od_sec2_kdf(const uint8_t k_in[16], const char *label,
                                      const uint8_t *ctx, uint16_t ctx_len, uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* OD_SEC2_KDF_H */
