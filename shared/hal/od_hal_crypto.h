/* od_hal_crypto.h -- the cryptography HAL every session-capable target implements.
 *
 * Link-time C functions, not a vtable: shared/ is plain C and binds to the target
 * implementation at link time (CLAUDE.md section "The one rule"). The panel bus
 * (od_epd_cfg_bus_t / od_epd_io_t) is the single deliberate function-pointer exception in this
 * tree, and this is not it.
 *
 * THE DIVISION OF LABOUR. The target owns its cryptographic engine, vendor key handles,
 * prepared-key storage and error logging. Shared session code owns the handshake, KDF inputs,
 * nonce construction, replay policy and integrity-strike policy. Vendor status values never
 * cross this boundary; the four-valued result preserves the policy distinctions shared/ needs.
 *
 * CONTEXT. Every function here is called from the dispatch pump, never from an ISR or stack
 * callback. The target may therefore use the synchronous form of its crypto API. Every entry
 * point self-initialises its backend idempotently; there is no separate init call and no required
 * ordering. In particular, random and CMAC must work before any prepared slot has been set.
 */
#ifndef OD_HAL_CRYPTO_H
#define OD_HAL_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* shared/ is plain C, but not every target is: ESP32 call sites are C++ translation units.
 * Without this guard the HAL declarations get C++ linkage and fail to match the target's C
 * definitions. The C-only host tests structurally cannot catch that mismatch. */
#ifdef __cplusplus
extern "C" {
#endif

enum od_hal_crypto_status {
    OD_HAL_CRYPTO_OK = 0,
    /* Decrypt only: the tag did not verify. This is never an engine/resource fault. */
    OD_HAL_CRYPTO_AUTH_FAILED,
    OD_HAL_CRYPTO_UNSUPPORTED,
    OD_HAL_CRYPTO_ERROR
};

/* The tag length belongs to the slot policy, not to an individual operation. PSA imports the
 * prepared key with this exact shortened-tag algorithm, so accepting a per-call length would
 * create different contracts on PSA and mbedTLS. */
#define OD_HAL_CRYPTO_TAG_LEN 12u

#ifndef OD_HAL_CRYPTO_KEY_SLOTS
#define OD_HAL_CRYPTO_KEY_SLOTS 1u
#endif

typedef uint8_t od_hal_crypto_slot_t;

/* Idempotent and self-repairing: release whatever the slot already holds before preparing key. */
enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot,
                                                const uint8_t key[16]);
void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot);

/* ct receives ciphertext || tag and must have room for
 * plain_len + OD_HAL_CRYPTO_TAG_LEN bytes. ct_len is set to that exact size on success. */
enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len);

/* ct is ciphertext || tag and must contain at least one ciphertext byte. AUTH_FAILED means only
 * that the tag mismatched. ON ANY NON-OK RETURN plain IS UNDEFINED AND MUST NOT BE READ: CCM is
 * decrypt-then-verify. plain_len is set to ct_len - OD_HAL_CRYPTO_TAG_LEN only on success. */
enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len);

/* One-shot, key-per-call operations. ECB exists only for KDF finalisation; it is not a general
 * encryption primitive for protocol payloads. */
enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16]);
enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16]);

/* A CSPRNG is mandatory because this fills the server nonce. Return ERROR if the backend cannot
 * provide cryptographically secure bytes; never substitute a weak source. */
enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len);

/* OD_CRYPTO_SOFT_CCM is deferred until a target lacks native CCM or native CCM has an
 * unacceptable flash cost. That implementation must add od_hal_crypto_ecb_prepared() so its
 * per-block primitive reuses a prepared key instead of importing the key for every AES block. */

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_CRYPTO_H */
