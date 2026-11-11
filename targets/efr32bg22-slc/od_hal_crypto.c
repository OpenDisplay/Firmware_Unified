/* Shared crypto HAL over Silicon Labs PSA/CRYPTOACC. */

#include "od_hal_crypto.h"
#include "od_diag.h"

#include <psa/crypto.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define OD_PSA_CCM_ALG PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OD_HAL_CRYPTO_TAG_LEN)

static psa_key_id_t s_keys[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_ready[OD_HAL_CRYPTO_KEY_SLOTS];
static bool s_psa_ready;

static bool valid_slot(od_hal_crypto_slot_t slot)
{
  return slot < OD_HAL_CRYPTO_KEY_SLOTS;
}

static bool init_once(void)
{
  if (!s_psa_ready) {
    s_psa_ready = psa_crypto_init() == PSA_SUCCESS;
  }
  return s_psa_ready;
}

static bool destroy_key(psa_key_id_t id)
{
  psa_status_t ps = psa_destroy_key(id);
  /* INVALID_HANDLE has the same postcondition: there is no key left to retire. */
  return ps == PSA_SUCCESS || ps == PSA_ERROR_INVALID_HANDLE;
}

static bool release_slot(od_hal_crypto_slot_t slot)
{
  psa_key_id_t id;

  if (!s_ready[slot]) {
    return true;
  }
  id = s_keys[slot];
  s_keys[slot] = 0;
  s_ready[slot] = false;
  if (!destroy_key(id)) {
    OD_DIAG("[OD] PSA session-key destroy failed; key slot leaked\r\n");
    return false;
  }
  return true;
}

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t slot,
                                                const uint8_t key[16])
{
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_status_t ps;

  if (!init_once() || !valid_slot(slot) || key == NULL || !release_slot(slot)) {
    return OD_HAL_CRYPTO_ERROR;
  }
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, 128);
  psa_set_key_algorithm(&attr, OD_PSA_CCM_ALG);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  ps = psa_import_key(&attr, key, 16u, &s_keys[slot]);
  psa_reset_key_attributes(&attr);
  if (ps != PSA_SUCCESS) {
    OD_DIAG("[OD] PSA CCM key import failed: %ld\r\n", (long)ps);
    return OD_HAL_CRYPTO_ERROR;
  }
  s_ready[slot] = true;
  return OD_HAL_CRYPTO_OK;
}

void od_hal_crypto_key_clear(od_hal_crypto_slot_t slot)
{
  if (init_once() && valid_slot(slot)) {
    (void)release_slot(slot);
  }
}

enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *plain, uint16_t plain_len, uint8_t *ct, uint16_t ct_cap,
        uint16_t *ct_len)
{
  size_t out_len = 0;
  psa_status_t ps;

  if (!init_once() || !valid_slot(slot) || !s_ready[slot] || nonce == NULL ||
      nonce_len < 7u || nonce_len > 13u || (aad == NULL && aad_len != 0u) ||
      (plain == NULL && plain_len != 0u) || ct == NULL || ct_len == NULL ||
      (uint32_t)plain_len + OD_HAL_CRYPTO_TAG_LEN > ct_cap) {
    return OD_HAL_CRYPTO_ERROR;
  }
  ps = psa_aead_encrypt(s_keys[slot], OD_PSA_CCM_ALG, nonce, nonce_len, aad, aad_len,
                        plain, plain_len, ct, ct_cap, &out_len);
  if (ps != PSA_SUCCESS || out_len != (size_t)plain_len + OD_HAL_CRYPTO_TAG_LEN) {
    OD_DIAG("[OD] PSA CCM encrypt failed: %ld\r\n", (long)ps);
    return OD_HAL_CRYPTO_ERROR;
  }
  *ct_len = (uint16_t)out_len;
  return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t slot,
        const uint8_t *nonce, uint8_t nonce_len, const uint8_t *aad, uint8_t aad_len,
        const uint8_t *ct, uint16_t ct_len, uint8_t *plain, uint16_t plain_cap,
        uint16_t *plain_len)
{
  size_t out_len = 0;
  uint16_t expected;
  psa_status_t ps;

  if (!init_once() || !valid_slot(slot) || !s_ready[slot] || nonce == NULL ||
      nonce_len < 7u || nonce_len > 13u || (aad == NULL && aad_len != 0u) ||
      ct == NULL || ct_len < OD_HAL_CRYPTO_TAG_LEN ||
      (plain == NULL && ct_len != OD_HAL_CRYPTO_TAG_LEN) || plain_len == NULL) {
    return OD_HAL_CRYPTO_ERROR;
  }
  expected = (uint16_t)(ct_len - OD_HAL_CRYPTO_TAG_LEN);
  if (plain_cap < expected) {
    return OD_HAL_CRYPTO_ERROR;
  }
  ps = psa_aead_decrypt(s_keys[slot], OD_PSA_CCM_ALG, nonce, nonce_len, aad, aad_len,
                        ct, ct_len, plain, plain_cap, &out_len);
  if (ps == PSA_ERROR_INVALID_SIGNATURE) {
    return OD_HAL_CRYPTO_AUTH_FAILED;
  }
  if (ps != PSA_SUCCESS || out_len != expected) {
    OD_DIAG("[OD] PSA CCM decrypt failed: %ld\r\n", (long)ps);
    return OD_HAL_CRYPTO_ERROR;
  }
  *plain_len = (uint16_t)out_len;
  return OD_HAL_CRYPTO_OK;
}

static enum od_hal_crypto_status one_shot(const uint8_t key[16], const uint8_t *input,
                                          uint32_t input_len, uint8_t out[16], bool cmac)
{
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  psa_key_id_t id = 0;
  psa_status_t ps;
  size_t out_len = 0;
  bool destroyed = true;

  if (!init_once() || key == NULL || input == NULL || out == NULL) {
    return OD_HAL_CRYPTO_ERROR;
  }
  psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attr, 128);
  psa_set_key_algorithm(&attr, cmac ? PSA_ALG_CMAC : PSA_ALG_ECB_NO_PADDING);
  psa_set_key_usage_flags(&attr, cmac ? PSA_KEY_USAGE_SIGN_MESSAGE : PSA_KEY_USAGE_ENCRYPT);
  ps = psa_import_key(&attr, key, 16u, &id);
  psa_reset_key_attributes(&attr);
  if (ps == PSA_SUCCESS) {
    if (cmac) {
      ps = psa_mac_compute(id, PSA_ALG_CMAC, input, input_len, out, 16u, &out_len);
    } else {
      ps = psa_cipher_encrypt(id, PSA_ALG_ECB_NO_PADDING, input, input_len,
                              out, 16u, &out_len);
    }
    destroyed = destroy_key(id);
  }
  if (ps != PSA_SUCCESS || out_len != 16u || !destroyed) {
    OD_DIAG("[OD] PSA %s failed: %ld%s\r\n", cmac ? "CMAC" : "ECB", (long)ps,
           destroyed ? "" : "; key slot leaked");
    return OD_HAL_CRYPTO_ERROR;
  }
  return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *msg,
                                             uint32_t msg_len, uint8_t out[16])
{
  if (msg == NULL && msg_len == 0u) {
    static const uint8_t empty = 0u;
    msg = &empty;
  }
  return one_shot(key, msg, msg_len, out, true);
}

enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t key[16], const uint8_t in[16],
                                                uint8_t out[16])
{
  return one_shot(key, in, 16u, out, false);
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
  if (!init_once() || (buf == NULL && len != 0u) || psa_generate_random(buf, len) != PSA_SUCCESS) {
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
    if (!destroy_key(id)) {
      OD_DIAG("[OD] PSA ECDH key destroy failed; key slot leaked\r\n");
    }
  }
}

enum od_hal_crypto_status od_hal_crypto_ecdh_keygen(uint8_t pub[64])
{
  psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
  uint8_t raw[65];
  size_t n = 0;
  psa_status_t ps;

  if (!init_once() || pub == NULL) {
    return OD_HAL_CRYPTO_ERROR;
  }
  od_hal_crypto_ecdh_discard();
  psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attr, 256);
  psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
  psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
  ps = psa_generate_key(&attr, &s_ecdh);
  psa_reset_key_attributes(&attr);
  if (ps != PSA_SUCCESS) {
    OD_DIAG("[OD] PSA ECDH keygen failed: %ld\r\n", (long)ps);
    return OD_HAL_CRYPTO_ERROR;
  }
  s_ecdh_ready = true;
  ps = psa_export_public_key(s_ecdh, raw, sizeof raw, &n);
  if (ps != PSA_SUCCESS || n != sizeof raw || raw[0] != 0x04u) {
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
  psa_status_t ps;

  if (!s_ecdh_ready || peer == NULL || z == NULL) {
    od_hal_crypto_ecdh_discard();
    return OD_HAL_CRYPTO_ERROR;
  }
  raw[0] = 0x04u;
  memcpy(&raw[1], peer, 64u);
  ps = psa_raw_key_agreement(PSA_ALG_ECDH, s_ecdh, raw, sizeof raw, z, 32u, &n);
  od_hal_crypto_ecdh_discard();
  if (ps == PSA_ERROR_INVALID_ARGUMENT) {
    return OD_HAL_CRYPTO_BAD_POINT;
  }
  return (ps == PSA_SUCCESS && n == 32u) ? OD_HAL_CRYPTO_OK : OD_HAL_CRYPTO_ERROR;
}
