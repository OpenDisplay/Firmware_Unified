/* session_fake.h -- the fake crypto + secret-store HAL and the shared fixture for every host
 * binary that links shared/core/od_sec2.
 *
 * The fake is bound at LINK time, exactly as a target's HAL is. AES is the host core
 * (tests/host/aes128.c, FIPS-197 checked by the aes128 suite); CMAC is RFC 4493 on top of it;
 * CCM is a generic RFC 3610 implementation (13-byte nonce, 12-byte tag, any AAD) written here;
 * P-256 is OpenSSL's, because nothing on the host is a reference P-256 otherwise.
 *
 * WHAT IT CANNOT PROVE: that PSA, mbedTLS or the Silabs CRYPTOACC driver is driven correctly,
 * that a real backend's tag matches this one, or anything about timing and stack. Those need
 * the targets.
 */

#ifndef OD_TEST_SESSION_FAKE_H
#define OD_TEST_SESSION_FAKE_H

#include "od_hal_crypto.h"
#include "od_hal_secret.h"
#include "opendisplay_structs.h"

#include <stdbool.h>
#include <stdint.h>

/* AES-CMAC (RFC 4493) on the host. */
void host_cmac(const uint8_t key[16], const uint8_t *msg, uint32_t len, uint8_t out[16]);

/* AES-CCM, 13-byte nonce, 12-byte tag, any AAD up to 255 bytes. out = C || T. */
void host_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                      uint8_t aad_len, const uint8_t *plain, uint16_t len, uint8_t *out);
/* in = C || T (len = |C| + 12). false on a tag mismatch; plain is then undefined. */
bool host_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                      uint8_t aad_len, const uint8_t *in, uint16_t len, uint8_t *plain);

/* P-256 over OpenSSL, for the fake device and the test client alike. tp256_new returns an
 * opaque key (NULL on failure) and its public point as X || Y. tp256_agree is ECDH; false when
 * OpenSSL rejects the peer point. */
void *tp256_new(uint8_t pub[64]);
bool  tp256_agree(void *key, const uint8_t peer[64], uint8_t z[32]);
void  tp256_free(void *key);

/* ------------------------------------------------------------------------- the fake's state --- */

/* Call before every case: clears slots, the ECDH key, the secret store and every injection. */
void fake_reset(void);

extern unsigned g_key_set_calls;
extern unsigned g_key_clear_calls;
extern uint8_t  g_slot_key[OD_HAL_CRYPTO_KEY_SLOTS][16];
bool fake_slot_loaded(unsigned slot);

/* Make HAL crypto calls fail with g_force_status once g_force_after calls have succeeded.
 * g_force_status == OK injects nothing; g_force_after == -1 injects nothing. */
extern enum od_hal_crypto_status g_force_status;
extern int32_t  g_force_after;
extern unsigned g_hal_calls;
/* true: the injection fires once and then clears g_force_status. */
extern bool g_force_once;

/* Deterministic RNG: bytes count up from g_rand_next. */
extern uint8_t g_rand_next;

/* ECDH: whether the device's ephemeral key is held; how many keygens ran. */
extern bool     g_ecdh_live;
extern unsigned g_ecdh_keygens;

/* The secret store: three blobs, a write counter, and a fault: the next g_secret_fail_after
 * successful writes go through, then writes fail (-1 = never). */
struct fake_secret {
    bool     present;
    uint16_t len;
    uint8_t  data[32];
};
extern struct fake_secret g_secret[3];
extern unsigned g_secret_writes;
extern int32_t  g_secret_fail_after;

/* ----------------------------------------------------------------------------- the fixture --- */

/* A 2.x key: od_sec2_boot(&g_sec) migrates it (row 4) to a CLAIMED, LEGACY device. */
extern const uint8_t MASTER[16];
extern const uint8_t UID[8];
extern struct SecurityConfig g_sec;

/* g_sec as a 2.x device stored it: encryption on, key MASTER, this timeout. */
void sec_init(uint16_t timeout_s);

#endif /* OD_TEST_SESSION_FAKE_H */
