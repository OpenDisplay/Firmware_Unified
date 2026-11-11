/* od_hal_crypto for ESP-IDF over mbedTLS. Contract: shared/hal/od_hal_crypto.h.
 *
 * od_hal_crypto_random() is NOT here: it lives in od_hal_crypto_random.c so a host test can
 * compile it against a fake RNG backend without dragging mbedTLS in. Same contract, same
 * translation-unit-local state, one file each. */

#include "od_hal_crypto.h"
#include "od_log.h"

#include "mbedtls/aes.h"
#include "mbedtls/ccm.h"
#include "mbedtls/cipher.h"
#include "mbedtls/cmac.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"

#include <string.h>

static mbedtls_ccm_context s_slots[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_slot_ready[OD_HAL_CRYPTO_KEY_SLOTS];

/* mbedTLS needs per-context initialisation but has no process-wide crypto init. Keeping the
 * idempotent backend hook explicit makes every public entry point satisfy the shared contract. */
static enum od_hal_crypto_status crypto_init_once(void)
{
    return OD_HAL_CRYPTO_OK;
}

static bool slot_valid(od_hal_crypto_slot_t slot)
{
    return slot < OD_HAL_CRYPTO_KEY_SLOTS;
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot,
                                                const uint8_t key[16])
{
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) || key == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (s_slot_ready[slot]) {
        mbedtls_ccm_free(&s_slots[slot]);
        s_slot_ready[slot] = false;
    }

    mbedtls_ccm_init(&s_slots[slot]);
    ret = mbedtls_ccm_setkey(&s_slots[slot], MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret != 0) {
        od_log_error("mbedTLS CCM setkey failed: %d", ret);
        mbedtls_ccm_free(&s_slots[slot]);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_slot_ready[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    (void)crypto_init_once();
    if (slot_valid(slot) && s_slot_ready[slot]) {
        mbedtls_ccm_free(&s_slots[slot]);
        s_slot_ready[slot] = false;
    }
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len)
{
    uint32_t required = (uint32_t)plain_len + OD_HAL_CRYPTO_TAG_LEN;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || (plain == NULL && plain_len != 0u) ||
        ct == NULL || ct_len == NULL || required > ct_cap) {
        return OD_HAL_CRYPTO_ERROR;
    }

    ret = mbedtls_ccm_encrypt_and_tag(&s_slots[slot], plain_len, nonce, nonce_len,
                                      aad, aad_len, plain, ct, &ct[plain_len],
                                      OD_HAL_CRYPTO_TAG_LEN);
    if (ret != 0) {
        od_log_error("mbedTLS CCM encrypt failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    *ct_len = (uint16_t)required;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len)
{
    uint16_t cipher_len;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || ct == NULL ||
        ct_len < OD_HAL_CRYPTO_TAG_LEN || (plain == NULL && ct_len != OD_HAL_CRYPTO_TAG_LEN) ||
        plain_len == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    cipher_len = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    if (plain_cap < cipher_len) {
        return OD_HAL_CRYPTO_ERROR;
    }

    ret = mbedtls_ccm_auth_decrypt(&s_slots[slot], cipher_len, nonce, nonce_len,
                                   aad, aad_len, ct, plain, &ct[cipher_len],
                                   OD_HAL_CRYPTO_TAG_LEN);
    if (ret == MBEDTLS_ERR_CCM_AUTH_FAILED) {
        return OD_HAL_CRYPTO_AUTH_FAILED;
    }
    if (ret != 0) {
        od_log_error("mbedTLS CCM decrypt failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    *plain_len = cipher_len;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    const mbedtls_cipher_info_t *cipher_info;
    mbedtls_cipher_context_t ctx;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL ||
        (msg == NULL && msg_len != 0u) || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    cipher_info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    if (cipher_info == NULL) {
        od_log_error("AES-128-ECB cipher info unavailable");
        return OD_HAL_CRYPTO_ERROR;
    }

    mbedtls_cipher_init(&ctx);
    ret = mbedtls_cipher_setup(&ctx, cipher_info);
    if (ret == 0) {
        ret = mbedtls_cipher_cmac_starts(&ctx, key, 128);
    }
    if (ret == 0) {
        ret = mbedtls_cipher_cmac_update(&ctx, msg, msg_len);
    }
    if (ret == 0) {
        ret = mbedtls_cipher_cmac_finish(&ctx, out);
    }
    mbedtls_cipher_free(&ctx);
    if (ret != 0) {
        od_log_error("mbedTLS CMAC failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    mbedtls_aes_context aes;
    int ret;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL || in == NULL || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    mbedtls_aes_init(&aes);
    ret = mbedtls_aes_setkey_enc(&aes, key, 128);
    if (ret == 0) {
        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
    }
    mbedtls_aes_free(&aes);
    if (ret != 0) {
        od_log_error("mbedTLS AES-ECB failed: %d", ret);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

/* ---- P-256 ECDH over mbedTLS ECP: one ephemeral key pair, destroyed by agree ---- */

static mbedtls_ecp_group s_grp;
static mbedtls_mpi s_d;
static bool s_ecdh_ready;

static int ecdh_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    while (len > 0u) {
        const uint16_t n = (len > 0xFFFFu) ? 0xFFFFu : (uint16_t)len;
        if (od_hal_crypto_random(buf, n) != OD_HAL_CRYPTO_OK) {
            return -1;
        }
        buf += n;
        len -= n;
    }
    return 0;
}

void od_hal_crypto_ecdh_discard(void)
{
    if (s_ecdh_ready) {
        mbedtls_mpi_free(&s_d);
        mbedtls_ecp_group_free(&s_grp);
        s_ecdh_ready = false;
    }
}

enum od_hal_crypto_status od_hal_crypto_ecdh_keygen(uint8_t pub[64])
{
    mbedtls_ecp_point q;
    uint8_t raw[65];
    size_t n = 0;
    int ret;

    if (pub == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    od_hal_crypto_ecdh_discard();
    mbedtls_ecp_group_init(&s_grp);
    mbedtls_mpi_init(&s_d);
    mbedtls_ecp_point_init(&q);
    s_ecdh_ready = true;
    ret = mbedtls_ecp_group_load(&s_grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret == 0) {
        ret = mbedtls_ecp_gen_keypair(&s_grp, &s_d, &q, ecdh_rng, NULL);
    }
    if (ret == 0) {
        ret = mbedtls_ecp_point_write_binary(&s_grp, &q, MBEDTLS_ECP_PF_UNCOMPRESSED, &n,
                                             raw, sizeof raw);
    }
    mbedtls_ecp_point_free(&q);
    if (ret != 0 || n != sizeof raw) {
        od_log_error("mbedTLS ECDH keygen failed: %d", ret);
        od_hal_crypto_ecdh_discard();
        return OD_HAL_CRYPTO_ERROR;
    }
    memcpy(pub, &raw[1], 64u);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ecdh_agree(const uint8_t peer[64], uint8_t z[32])
{
    enum od_hal_crypto_status st = OD_HAL_CRYPTO_ERROR;
    mbedtls_ecp_point p;
    mbedtls_mpi zz;
    uint8_t raw[65];
    int ret;

    if (!s_ecdh_ready || peer == NULL || z == NULL) {
        od_hal_crypto_ecdh_discard();
        return OD_HAL_CRYPTO_ERROR;
    }
    mbedtls_ecp_point_init(&p);
    mbedtls_mpi_init(&zz);
    raw[0] = 0x04u;
    memcpy(&raw[1], peer, 64u);
    ret = mbedtls_ecp_point_read_binary(&s_grp, &p, raw, sizeof raw);
    if (ret == 0) {
        ret = mbedtls_ecp_check_pubkey(&s_grp, &p);
        if (ret != 0) {
            st = OD_HAL_CRYPTO_BAD_POINT;
            goto cleanup;
        }
        ret = mbedtls_ecdh_compute_shared(&s_grp, &zz, &p, &s_d, ecdh_rng, NULL);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&zz, z, 32u);
    }
    st = (ret == 0) ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_ERROR;
cleanup:
    mbedtls_ecp_point_free(&p);
    mbedtls_mpi_free(&zz);
    od_hal_crypto_ecdh_discard();
    return st;
}
