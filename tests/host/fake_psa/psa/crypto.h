/* psa/crypto.h -- a host stand-in for the PSA Crypto API, enough of it to COMPILE AND LINK the
 * targets' production od_hal_crypto sources on a workstation.
 *
 * WHY A FAKE VENDOR HEADER RATHER THAN A REIMPLEMENTATION. The defects these suites cover are
 * lifecycle defects -- which key id is destroyed, when a tracked slot is cleared, whether a
 * failure propagates -- and a test that re-implements the logic proves only that the copy agrees
 * with itself. Compiling the real file against this header exercises the shipped control flow.
 *
 * NOT A CRYPTO IMPLEMENTATION. Keys are recorded, not used; AEAD and MAC produce deterministic
 * filler. Nothing here belongs anywhere near a device, and nothing here is on any target's include
 * path -- tests/host/fake_psa/ is added only to the host binaries that need it.
 */

#ifndef OD_TEST_FAKE_PSA_CRYPTO_H
#define OD_TEST_FAKE_PSA_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t  psa_status_t;
typedef uint32_t psa_key_id_t;
typedef uint32_t psa_key_type_t;
typedef uint32_t psa_algorithm_t;
typedef uint32_t psa_key_usage_t;

#define PSA_SUCCESS                    ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR        ((psa_status_t)-132)
#define PSA_ERROR_NOT_PERMITTED        ((psa_status_t)-133)
#define PSA_ERROR_INVALID_SIGNATURE    ((psa_status_t)-149)
#define PSA_ERROR_INVALID_ARGUMENT     ((psa_status_t)-135)
#define PSA_ERROR_INSUFFICIENT_MEMORY  ((psa_status_t)-141)

#define PSA_KEY_TYPE_AES               ((psa_key_type_t)0x2400)

#define PSA_ALG_CCM                    ((psa_algorithm_t)0x05500100)
#define PSA_ALG_CMAC                   ((psa_algorithm_t)0x03c00200)
#define PSA_ALG_ECB_NO_PADDING         ((psa_algorithm_t)0x04404400)

/* The real macro splices the tag length into the algorithm word; the exact encoding does not
 * matter here, only that a different length is a different algorithm value -- which is what makes
 * the shortened-tag policy visible to a test at all. */
#define PSA_ALG_AEAD_WITH_SHORTENED_TAG(alg, tag_len) \
    ((psa_algorithm_t)(((alg) & ~0x003F0000u) | (((tag_len) & 0x3Fu) << 16)))

#define PSA_KEY_USAGE_ENCRYPT          ((psa_key_usage_t)0x00000100)
#define PSA_KEY_USAGE_DECRYPT          ((psa_key_usage_t)0x00000200)
#define PSA_KEY_USAGE_SIGN_MESSAGE     ((psa_key_usage_t)0x00000400)
#define PSA_KEY_USAGE_VERIFY_MESSAGE   ((psa_key_usage_t)0x00000800)
#define PSA_KEY_USAGE_DERIVE           ((psa_key_usage_t)0x00004000)

typedef uint8_t psa_ecc_family_t;
#define PSA_ECC_FAMILY_SECP_R1         ((psa_ecc_family_t)0x12)
#define PSA_KEY_TYPE_ECC_KEY_PAIR(curve) ((psa_key_type_t)(0x7100u | (curve)))
#define PSA_ALG_ECDH                   ((psa_algorithm_t)0x09020000)

typedef struct {
    psa_key_type_t  type;
    size_t          bits;
    psa_algorithm_t alg;
    psa_key_usage_t usage;
} psa_key_attributes_t;

#define PSA_KEY_ATTRIBUTES_INIT { 0, 0, 0, 0 }

void psa_set_key_type(psa_key_attributes_t *attr, psa_key_type_t type);
void psa_set_key_bits(psa_key_attributes_t *attr, size_t bits);
void psa_set_key_algorithm(psa_key_attributes_t *attr, psa_algorithm_t alg);
void psa_set_key_usage_flags(psa_key_attributes_t *attr, psa_key_usage_t usage);
void psa_reset_key_attributes(psa_key_attributes_t *attr);

psa_status_t psa_crypto_init(void);

psa_status_t psa_import_key(const psa_key_attributes_t *attr, const uint8_t *data,
                            size_t data_len, psa_key_id_t *key);
psa_status_t psa_destroy_key(psa_key_id_t key);

psa_status_t psa_aead_encrypt(psa_key_id_t key, psa_algorithm_t alg,
                              const uint8_t *nonce, size_t nonce_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *ct, size_t ct_size, size_t *ct_len);
psa_status_t psa_aead_decrypt(psa_key_id_t key, psa_algorithm_t alg,
                              const uint8_t *nonce, size_t nonce_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *ct, size_t ct_len,
                              uint8_t *plain, size_t plain_size, size_t *plain_len);
psa_status_t psa_mac_compute(psa_key_id_t key, psa_algorithm_t alg,
                             const uint8_t *msg, size_t msg_len,
                             uint8_t *mac, size_t mac_size, size_t *mac_len);
psa_status_t psa_cipher_encrypt(psa_key_id_t key, psa_algorithm_t alg,
                                const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t out_size, size_t *out_len);
psa_status_t psa_generate_random(uint8_t *out, size_t out_len);

/* ECDH, structurally only: the fake checks the 04 prefix and refuses an all-zero X with
 * INVALID_ARGUMENT, and the "shared secret" is a byte mix, not P-256. */
psa_status_t psa_generate_key(const psa_key_attributes_t *attr, psa_key_id_t *key);
psa_status_t psa_export_public_key(psa_key_id_t key, uint8_t *data, size_t data_size,
                                   size_t *data_len);
psa_status_t psa_raw_key_agreement(psa_algorithm_t alg, psa_key_id_t key,
                                   const uint8_t *peer, size_t peer_len,
                                   uint8_t *out, size_t out_size, size_t *out_len);

#endif /* OD_TEST_FAKE_PSA_CRYPTO_H */
