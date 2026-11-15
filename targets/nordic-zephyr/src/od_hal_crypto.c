/* od_hal_crypto for Zephyr on nRF over PSA Crypto. Contract: shared/hal/od_hal_crypto.h. */

#include "od_hal_crypto.h"
#include "od_log.h"

#include <psa/crypto.h>
#include <string.h>

#define OD_PSA_CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OD_HAL_CRYPTO_TAG_LEN)

static psa_key_id_t s_slots[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_slot_ready[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_crypto_ready;

static enum od_hal_crypto_status crypto_init_once(void)
{
    psa_status_t status;

    if (s_crypto_ready) {
        return OD_HAL_CRYPTO_OK;
    }
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA init failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_crypto_ready = true;
    return OD_HAL_CRYPTO_OK;
}

static bool slot_valid(od_hal_crypto_slot_t slot)
{
    return slot < OD_HAL_CRYPTO_KEY_SLOTS;
}

/* OWNERSHIP IS DROPPED BEFORE PSA IS ASKED, and the order is the whole of the fix.
 *
 * Clearing the tracked id only after a SUCCESSFUL destroy latches the slot: one destroy error
 * leaves it marked ready, every later key_set() retries the same failing release, and the device
 * cannot authenticate again until it reboots. From the host's side that is a brick.
 *
 * The failed id is then LEAKED DELIBERATELY, not held for a retry. A parked id is a hazard -- PSA
 * may reissue that number to another key, and a delayed destroy would free somebody else's. The
 * handshake that met the failure still fails; the next one starts clean. Repeated occurrences in
 * a log ARE the finding: destroying a volatile key imported seconds ago should not fail, and if it
 * does the pool is draining. */
static enum od_hal_crypto_status slot_release(od_hal_crypto_slot_t slot)
{
    psa_key_id_t id;
    psa_status_t status;

    if (!s_slot_ready[slot]) {
        return OD_HAL_CRYPTO_OK;
    }
    id = s_slots[slot];
    s_slots[slot] = 0;
    s_slot_ready[slot] = false;

    status = psa_destroy_key(id);
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA key destroy failed: %ld - slot leaked", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot,
                                                const uint8_t key[16])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) || key == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (slot_release(slot) != OD_HAL_CRYPTO_OK) {
        return OD_HAL_CRYPTO_ERROR;
    }

    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    /* The key policy includes the 12-byte tag. Plain PSA_ALG_CCM pins a 16-byte tag and rejects
     * every wire-compatible operation with PSA_ERROR_NOT_PERMITTED. */
    psa_set_key_algorithm(&attr, OD_PSA_CCM_ALG);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    status = psa_import_key(&attr, key, 16, &s_slots[slot]);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA CCM key import failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_slot_ready[slot] = true;
    return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
    if (crypto_init_once() == OD_HAL_CRYPTO_OK && slot_valid(slot)) {
        (void)slot_release(slot);
    }
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len,
        uint8_t *ct, uint16_t ct_cap, uint16_t *ct_len)
{
    size_t output_len = 0;
    uint32_t required = (uint32_t)plain_len + OD_HAL_CRYPTO_TAG_LEN;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || plain == NULL ||
        ct == NULL || ct_len == NULL || required > ct_cap) {
        return OD_HAL_CRYPTO_ERROR;
    }

    status = psa_aead_encrypt(s_slots[slot], OD_PSA_CCM_ALG, nonce, nonce_len,
                              aad, aad_len, plain, plain_len, ct, ct_cap, &output_len);
    if (status != PSA_SUCCESS || output_len != required) {
        od_log_error("crypto: PSA CCM encrypt failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    *ct_len = (uint16_t)output_len;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len,
        const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len,
        uint8_t *plain, uint16_t plain_cap, uint16_t *plain_len)
{
    size_t output_len = 0;
    uint16_t expected_len;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || !slot_valid(slot) ||
        !s_slot_ready[slot] || nonce == NULL || nonce_len < 7u || nonce_len > 13u ||
        (aad == NULL && aad_len != 0u) || ct == NULL ||
        ct_len < OD_HAL_CRYPTO_TAG_LEN || plain == NULL ||
        plain_len == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    expected_len = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
    if (plain_cap < expected_len) {
        return OD_HAL_CRYPTO_ERROR;
    }

    status = psa_aead_decrypt(s_slots[slot], OD_PSA_CCM_ALG, nonce, nonce_len,
                              aad, aad_len, ct, ct_len, plain, plain_cap, &output_len);
    if (status == PSA_ERROR_INVALID_SIGNATURE) {
        return OD_HAL_CRYPTO_AUTH_FAILED;
    }
    if (status != PSA_SUCCESS || output_len != expected_len) {
        od_log_error("crypto: PSA CCM decrypt failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    *plain_len = (uint16_t)output_len;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t mac_len = 0;
    psa_status_t status;
    psa_status_t destroy_status = PSA_SUCCESS;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL ||
        (msg == NULL && msg_len != 0u) || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_algorithm(&attr, PSA_ALG_CMAC);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    status = psa_import_key(&attr, key, 16, &key_id);
    psa_reset_key_attributes(&attr);
    if (status == PSA_SUCCESS) {
        status = psa_mac_compute(key_id, PSA_ALG_CMAC, msg, msg_len, out, 16, &mac_len);
        destroy_status = psa_destroy_key(key_id);   /* kept: see the report below */
    }
    /* A failed destroy leaks this PSA slot: the id was a local and goes with it. Accepted, not
     * repaired -- retrying needs the id held somewhere, and PSA may reissue a held id to another
     * key that the retry would then destroy. Checked before the operation status so a cleanup
     * failure is still visible when the operation ALSO failed, which is the case worth reading. */
    if (destroy_status != PSA_SUCCESS) {
        od_log_error("crypto: PSA key destroy after CMAC failed: %ld - slot leaked",
                     (long)destroy_status);
    }
    if (status != PSA_SUCCESS || mac_len != 16u) {
        od_log_error("crypto: PSA CMAC failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    if (destroy_status != PSA_SUCCESS) {
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    size_t out_len = 0;
    psa_status_t status;
    psa_status_t destroy_status = PSA_SUCCESS;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || key == NULL || in == NULL || out == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_algorithm(&attr, PSA_ALG_ECB_NO_PADDING);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT);
    status = psa_import_key(&attr, key, 16, &key_id);
    psa_reset_key_attributes(&attr);
    if (status == PSA_SUCCESS) {
        status = psa_cipher_encrypt(key_id, PSA_ALG_ECB_NO_PADDING, in, 16, out, 16, &out_len);
        destroy_status = psa_destroy_key(key_id);   /* kept: see the report below */
    }
    if (destroy_status != PSA_SUCCESS) {
        /* The key id was a local and is gone with it, so this slot is leaked. That is
         * deliberate: retrying a destroy needs the id kept somewhere, and a holding pen
         * for it costs more than it saves -- it can wedge the driver shut on statuses
         * that never resolve, and a parked id may be reissued by PSA to another key,
         * which a retry would then destroy. Report it and move on. Repeated occurrences
         * in a log ARE the finding: destroying a volatile key we imported seconds ago
         * should not fail, and if it does the pool is draining. */
        od_log_error("crypto: PSA key destroy after AES-ECB failed: %ld - slot leaked",
                     (long)destroy_status);
    }
    if (status != PSA_SUCCESS || out_len != 16u) {
        od_log_error("crypto: PSA AES-ECB failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    if (destroy_status != PSA_SUCCESS) {
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || (buf == NULL && len != 0u)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    status = psa_generate_random(buf, len);
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA random failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}

/* ---- P-256 ECDH: one volatile key pair, generated per handshake and destroyed by agree ---- */

static psa_key_id_t s_ecdh;
static bool s_ecdh_ready;

void od_hal_crypto_ecdh_discard(void)
{
    if (s_ecdh_ready) {
        const psa_key_id_t id = s_ecdh;

        s_ecdh = 0;
        s_ecdh_ready = false;
        if (psa_destroy_key(id) != PSA_SUCCESS) {
            od_log_error("crypto: PSA ECDH key destroy failed - slot leaked");
        }
    }
}

enum od_hal_crypto_status od_hal_crypto_ecdh_keygen(uint8_t pub[64])
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    uint8_t raw[65];
    size_t n = 0;
    psa_status_t status;

    if (crypto_init_once() != OD_HAL_CRYPTO_OK || pub == NULL) {
        return OD_HAL_CRYPTO_ERROR;
    }
    od_hal_crypto_ecdh_discard();
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    status = psa_generate_key(&attr, &s_ecdh);
    psa_reset_key_attributes(&attr);
    if (status != PSA_SUCCESS) {
        od_log_error("crypto: PSA ECDH keygen failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_ecdh_ready = true;
    status = psa_export_public_key(s_ecdh, raw, sizeof raw, &n);
    if (status != PSA_SUCCESS || n != sizeof raw || raw[0] != 0x04u) {
        od_log_error("crypto: PSA ECDH export failed: %ld", (long)status);
        od_hal_crypto_ecdh_discard();
        return OD_HAL_CRYPTO_ERROR;
    }
    memcpy(pub, &raw[1], 64u);
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ecdh_agree(const uint8_t peer[64], uint8_t z[32])
{
    uint8_t raw[65];
    size_t n = 0;
    psa_status_t status;

    if (!s_ecdh_ready || peer == NULL || z == NULL) {
        od_hal_crypto_ecdh_discard();
        return OD_HAL_CRYPTO_ERROR;
    }
    raw[0] = 0x04u;
    memcpy(&raw[1], peer, 64u);
    status = psa_raw_key_agreement(PSA_ALG_ECDH, s_ecdh, raw, sizeof raw, z, 32u, &n);
    od_hal_crypto_ecdh_discard();
    if (status == PSA_ERROR_INVALID_ARGUMENT) {
        return OD_HAL_CRYPTO_BAD_POINT;
    }
    if (status != PSA_SUCCESS || n != 32u) {
        od_log_error("crypto: PSA ECDH agree failed: %ld", (long)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}
