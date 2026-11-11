/* aes128.h -- AES-128 block encryption for the host test suite. TEST CODE ONLY.
 *
 * WHY THIS EXISTS. tests/host/ links nothing but od_shared: there is no crypto library on the
 * host at all. The target backends are mbedTLS (esp32-idf) and PSA (nordic-zephyr), neither of
 * which exists in a host build, so without this the SEC2 tests cannot compute a single
 * expected byte. Two consumers need it:
 *
 *   - the fake od_hal_crypto (session_fake.c), which must implement cmac/aes_ecb/ccm for real. A
 *     stub cannot exercise the SEC2 KDF (CMAC) and cannot produce a CCM tag od_sec2 will accept.
 *   - the SEC2 test client (sec2_client.c), which computes every expected tag on the same core.
 *
 * NEVER LINKED INTO FIRMWARE. No target builds this file, and it must not acquire a consumer
 * under targets/ or shared/ -- the targets have hardware-accelerated implementations and this
 * one is written for clarity, not for constant time or speed.
 *
 * PRUNED FORM, decided before the first commit (MIGRATION.md "Risks to watch"). ENCRYPTION AND
 * 128-BIT KEYS ONLY: no decryption, no 192/256-bit keys, no cipher modes. AES-CMAC, AES-ECB and
 * CCM all use the forward direction exclusively, so a decrypt path would be untested code on the
 * one surface where untested code is least welcome.
 *
 * PROVENANCE. A direct implementation of FIPS-197 (the AES specification), written for this
 * repo rather than copied from a third-party tree -- which is why it carries this repo's licence
 * and has no entry in third_party/NOTICE.md. The S-box and round-constant tables are the
 * standard's own published constants and are not copyrightable expression. Verified against the
 * FIPS-197 Appendix B/C.1 known-answer vectors in aes128_test.c.
 */
#ifndef OD_TEST_AES128_H
#define OD_TEST_AES128_H

#include <stdint.h>

/* Encrypt exactly one 16-byte block. in and out may alias. */
void od_test_aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif /* OD_TEST_AES128_H */
