/* fake_psa.c -- see fake_psa.h. Key ids are handed out MONOTONICALLY and never recycled, which is
 * deliberate: it makes "the implementation destroyed an id it should no longer own" observable,
 * where a recycling allocator would hide it behind a coincidence. */

#include "fake_psa.h"

#include <string.h>

#define FAKE_PSA_MAX_KEYS 16u

static struct {
    psa_key_id_t         id;
    bool                 live;
    uint8_t              key[32];
    size_t               key_len;
    psa_key_attributes_t attr;
} s_keys[FAKE_PSA_MAX_KEYS];

static psa_key_id_t s_next_id;

unsigned    fake_psa_init_calls;
unsigned    fake_psa_import_calls;
unsigned    fake_psa_destroy_calls;
unsigned    fake_psa_random_calls;
psa_key_id_t fake_psa_last_destroyed;
psa_key_id_t fake_psa_destroy_log[FAKE_PSA_DESTROY_LOG_MAX];
unsigned     fake_psa_destroy_log_n;
psa_key_attributes_t fake_psa_last_attr;

unsigned     fake_psa_fail_init_on;
psa_status_t fake_psa_fail_init_status;
unsigned     fake_psa_fail_import_on;
psa_status_t fake_psa_fail_import_status;
unsigned     fake_psa_fail_destroy_on;
psa_status_t fake_psa_fail_destroy_status;
unsigned     fake_psa_fail_random_on;
psa_status_t fake_psa_fail_random_status;
bool         fake_psa_random_short;

void fake_psa_reset(void)
{
    memset(s_keys, 0, sizeof s_keys);
    s_next_id = 0x40000001u;          /* nothing meaningful, just clearly not 0 */

    fake_psa_init_calls = 0u;
    fake_psa_import_calls = 0u;
    fake_psa_destroy_calls = 0u;
    fake_psa_random_calls = 0u;
    fake_psa_last_destroyed = 0u;
    fake_psa_destroy_log_n = 0u;
    memset(fake_psa_destroy_log, 0, sizeof fake_psa_destroy_log);
    memset(&fake_psa_last_attr, 0, sizeof fake_psa_last_attr);

    fake_psa_fail_init_on = 0u;
    fake_psa_fail_init_status = PSA_ERROR_GENERIC_ERROR;
    fake_psa_fail_import_on = 0u;
    fake_psa_fail_import_status = PSA_ERROR_GENERIC_ERROR;
    fake_psa_fail_destroy_on = 0u;
    fake_psa_fail_destroy_status = PSA_ERROR_GENERIC_ERROR;
    fake_psa_fail_random_on = 0u;
    fake_psa_fail_random_status = PSA_ERROR_GENERIC_ERROR;
    fake_psa_random_short = false;
}

bool fake_psa_key_live(psa_key_id_t id)
{
    unsigned i;

    for (i = 0u; i < FAKE_PSA_MAX_KEYS; ++i) {
        if (s_keys[i].live && s_keys[i].id == id) {
            return true;
        }
    }
    return false;
}

unsigned fake_psa_live_keys(void)
{
    unsigned i, n = 0u;

    for (i = 0u; i < FAKE_PSA_MAX_KEYS; ++i) {
        if (s_keys[i].live) {
            ++n;
        }
    }
    return n;
}

/* --------------------------------------------------------------------------- attributes --- */

void psa_set_key_type(psa_key_attributes_t *attr, psa_key_type_t type)
{
    if (attr != NULL) { attr->type = type; }
}
void psa_set_key_bits(psa_key_attributes_t *attr, size_t bits)
{
    if (attr != NULL) { attr->bits = bits; }
}
void psa_set_key_algorithm(psa_key_attributes_t *attr, psa_algorithm_t alg)
{
    if (attr != NULL) { attr->alg = alg; }
}
void psa_set_key_usage_flags(psa_key_attributes_t *attr, psa_key_usage_t usage)
{
    if (attr != NULL) { attr->usage = usage; }
}
void psa_reset_key_attributes(psa_key_attributes_t *attr)
{
    if (attr != NULL) { memset(attr, 0, sizeof *attr); }
}

/* --------------------------------------------------------------------------- entry points --- */

psa_status_t psa_crypto_init(void)
{
    ++fake_psa_init_calls;
    if (fake_psa_fail_init_on == fake_psa_init_calls) {
        return fake_psa_fail_init_status;
    }
    return PSA_SUCCESS;
}

psa_status_t psa_import_key(const psa_key_attributes_t *attr, const uint8_t *data,
                            size_t data_len, psa_key_id_t *key)
{
    unsigned i;

    ++fake_psa_import_calls;
    if (fake_psa_fail_import_on == fake_psa_import_calls) {
        return fake_psa_fail_import_status;
    }
    if (attr == NULL || data == NULL || key == NULL ||
        data_len == 0u || data_len > sizeof s_keys[0].key) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < FAKE_PSA_MAX_KEYS; ++i) {
        if (!s_keys[i].live) {
            break;
        }
    }
    if (i == FAKE_PSA_MAX_KEYS) {
        return PSA_ERROR_INSUFFICIENT_MEMORY;
    }
    s_keys[i].id = s_next_id++;
    s_keys[i].live = true;
    s_keys[i].key_len = data_len;
    memcpy(s_keys[i].key, data, data_len);
    s_keys[i].attr = *attr;
    fake_psa_last_attr = *attr;
    *key = s_keys[i].id;
    return PSA_SUCCESS;
}

psa_status_t psa_destroy_key(psa_key_id_t key)
{
    unsigned i;

    ++fake_psa_destroy_calls;
    fake_psa_last_destroyed = key;
    if (fake_psa_destroy_log_n < FAKE_PSA_DESTROY_LOG_MAX) {
        fake_psa_destroy_log[fake_psa_destroy_log_n++] = key;
    }
    if (fake_psa_fail_destroy_on == fake_psa_destroy_calls) {
        /* The key SURVIVES a failed destroy, which is the whole point: the caller's tracking must
         * cope with a slot it can no longer name and can no longer free. */
        return fake_psa_fail_destroy_status;
    }
    for (i = 0u; i < FAKE_PSA_MAX_KEYS; ++i) {
        if (s_keys[i].live && s_keys[i].id == key) {
            memset(&s_keys[i], 0, sizeof s_keys[i]);
            return PSA_SUCCESS;
        }
    }
    /* PSA treats destroying an absent key as success (it is already gone). */
    return PSA_SUCCESS;
}

/* The three cryptographic entry points below are shape-only: they check the arguments a caller
 * must get right and then produce deterministic filler. No test asserts on the bytes. */

psa_status_t psa_aead_encrypt(psa_key_id_t key, psa_algorithm_t alg,
                              const uint8_t *nonce, size_t nonce_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *plain, size_t plain_len,
                              uint8_t *ct, size_t ct_size, size_t *ct_len)
{
    size_t need = plain_len + 12u;

    (void)alg; (void)nonce; (void)nonce_len; (void)aad; (void)aad_len;
    if (!fake_psa_key_live(key) || ct == NULL || ct_len == NULL || ct_size < need) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (plain_len != 0u && plain != NULL) {
        memcpy(ct, plain, plain_len);
    }
    memset(ct + plain_len, 0xA5, 12u);
    *ct_len = need;
    return PSA_SUCCESS;
}

psa_status_t psa_aead_decrypt(psa_key_id_t key, psa_algorithm_t alg,
                              const uint8_t *nonce, size_t nonce_len,
                              const uint8_t *aad, size_t aad_len,
                              const uint8_t *ct, size_t ct_len,
                              uint8_t *plain, size_t plain_size, size_t *plain_len)
{
    size_t out;

    (void)alg; (void)nonce; (void)nonce_len; (void)aad; (void)aad_len;
    if (!fake_psa_key_live(key) || ct == NULL || plain == NULL || plain_len == NULL ||
        ct_len < 12u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    out = ct_len - 12u;
    if (plain_size < out) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (out != 0u) {
        memcpy(plain, ct, out);
    }
    *plain_len = out;
    return PSA_SUCCESS;
}

psa_status_t psa_mac_compute(psa_key_id_t key, psa_algorithm_t alg,
                             const uint8_t *msg, size_t msg_len,
                             uint8_t *mac, size_t mac_size, size_t *mac_len)
{
    (void)alg; (void)msg; (void)msg_len;
    if (!fake_psa_key_live(key) || mac == NULL || mac_len == NULL || mac_size < 16u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    memset(mac, 0x5A, 16u);
    *mac_len = 16u;
    return PSA_SUCCESS;
}

psa_status_t psa_cipher_encrypt(psa_key_id_t key, psa_algorithm_t alg,
                                const uint8_t *in, size_t in_len,
                                uint8_t *out, size_t out_size, size_t *out_len)
{
    (void)alg;
    if (!fake_psa_key_live(key) || in == NULL || out == NULL || out_len == NULL ||
        out_size < in_len) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    memcpy(out, in, in_len);
    *out_len = in_len;
    return PSA_SUCCESS;
}

psa_status_t psa_generate_random(uint8_t *out, size_t out_len)
{
    size_t i;

    ++fake_psa_random_calls;
    if (fake_psa_fail_random_on == fake_psa_random_calls) {
        return fake_psa_fail_random_status;
    }
    if (out == NULL && out_len != 0u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (fake_psa_random_short && out_len > 1u) {
        out_len -= 1u;
    }
    for (i = 0u; i < out_len; ++i) {
        out[i] = (uint8_t)(0x11u + i);
    }
    return PSA_SUCCESS;
}

psa_status_t psa_generate_key(const psa_key_attributes_t *attr, psa_key_id_t *key)
{
    uint8_t priv[32];
    unsigned i;

    for (i = 0u; i < sizeof priv; ++i) {
        priv[i] = (uint8_t)(s_next_id + i);
    }
    return psa_import_key(attr, priv, sizeof priv, key);
}

static const uint8_t *key_bytes(psa_key_id_t id)
{
    unsigned i;

    for (i = 0u; i < FAKE_PSA_MAX_KEYS; ++i) {
        if (s_keys[i].live && s_keys[i].id == id) {
            return s_keys[i].key;
        }
    }
    return NULL;
}

psa_status_t psa_export_public_key(psa_key_id_t key, uint8_t *data, size_t data_size,
                                   size_t *data_len)
{
    const uint8_t *k = key_bytes(key);
    unsigned i;

    if (k == NULL || data == NULL || data_len == NULL || data_size < 65u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    data[0] = 0x04u;
    for (i = 0u; i < 64u; ++i) {
        data[1u + i] = (uint8_t)(k[i % 32u] ^ 0xA5u ^ i);
    }
    *data_len = 65u;
    return PSA_SUCCESS;
}

psa_status_t psa_raw_key_agreement(psa_algorithm_t alg, psa_key_id_t key,
                                   const uint8_t *peer, size_t peer_len,
                                   uint8_t *out, size_t out_size, size_t *out_len)
{
    const uint8_t *k = key_bytes(key);
    uint8_t any = 0u;
    unsigned i;

    if (alg != PSA_ALG_ECDH || k == NULL || peer == NULL || peer_len != 65u || peer[0] != 0x04u ||
        out == NULL || out_len == NULL || out_size < 32u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < 32u; ++i) {
        any |= peer[1u + i];
    }
    if (any == 0u) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0u; i < 32u; ++i) {
        out[i] = (uint8_t)(k[i] ^ peer[1u + i]);
    }
    *out_len = 32u;
    return PSA_SUCCESS;
}
