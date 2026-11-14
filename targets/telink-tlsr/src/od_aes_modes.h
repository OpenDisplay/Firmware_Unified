/* AES-CMAC (RFC 4493) and AES-CCM (RFC 3610) over a caller-supplied AES-128 block encryptor.
 *
 * The TLSR825x has an AES-ECB engine and nothing above it, so the modes live here. Plain C with
 * no SDK dependency, so tests/host/tlsr_aes_modes_test.c runs the same file against the RFC
 * vectors and the repo's reference CCM. */
#ifndef OD_AES_MODES_H
#define OD_AES_MODES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encrypt one 16-byte block under a 128-bit key. false = the engine failed; nothing is trusted. */
typedef bool (*od_aes_block_fn)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

bool od_aes_cmac(od_aes_block_fn enc, const uint8_t key[16], const uint8_t *msg, uint32_t len,
                 uint8_t out[16]);

/* nonce_len 7..13, tag_len 4..16 and even, aad_len < 0xFF00. `out` receives ciphertext followed
 * by the tag, plain_len + tag_len bytes; it may alias `plain`. */
bool od_aes_ccm_encrypt(od_aes_block_fn enc, const uint8_t key[16],
                        const uint8_t *nonce, uint8_t nonce_len,
                        const uint8_t *aad, uint16_t aad_len,
                        const uint8_t *plain, uint16_t plain_len,
                        uint8_t tag_len, uint8_t *out);

/* `in` is ciphertext followed by the tag (ct_len includes the tag). Writes ct_len - tag_len
 * bytes to `plain`, which may alias `in`. On a tag mismatch `plain` is zeroed and
 * *auth_ok = false; the return value only reports engine failure or bad parameters. */
bool od_aes_ccm_decrypt(od_aes_block_fn enc, const uint8_t key[16],
                        const uint8_t *nonce, uint8_t nonce_len,
                        const uint8_t *aad, uint16_t aad_len,
                        const uint8_t *in, uint16_t ct_len,
                        uint8_t tag_len, uint8_t *plain, bool *auth_ok);

#ifdef __cplusplus
}
#endif

#endif /* OD_AES_MODES_H */
