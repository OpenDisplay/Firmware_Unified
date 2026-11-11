/* od_hal_crypto_random -- this target's RNG arm of shared/hal/od_hal_crypto.h.
 *
 * Its own translation unit so tests/host can compile the production function against a fake PSA;
 * the rest of the HAL is mbedTLS and does not build on a workstation.
 *
 * PSA RATHER THAN esp_fill_random(), FOR ONE REASON: esp_fill_random() returns void. An adapter
 * built on it can only ever report success, so the session rule -- never offer a nonce the
 * device cannot honour -- has nothing to act on, and a failed draw becomes a challenge that is
 * predictable or stale. psa_generate_random() returns a status, so the failure reaches the caller
 * and the handshake answers AUTH_STATUS_ERROR instead.
 *
 * THE AEAD STAYS ON CLASSIC MBEDTLS. Initialising PSA here makes routing CCM through psa_aead_*
 * look like a small follow-on. It is not: that is a second crypto-backend migration with its own
 * hardware gate, and Nordic's PSA arm is itself still unverified on silicon.
 *
 * NO FALLBACK. A PSA error must not quietly drop back to esp_fill_random(): the caller would then
 * get a success indistinguishable from a real one, which is the behaviour this file exists to
 * remove.
 *
 * No sdkconfig change backs this. ESP-IDF exposes no Kconfig symbol for MBEDTLS_PSA_CRYPTO_C and
 * its port neither defines nor undefines it, so the setting comes from upstream Mbed TLS's own
 * default -- absence from sdkconfig.h is not evidence of absence from the build. Confirmed by
 * inspecting the built archive on a WiFi board and a non-WiFi one alike:
 *
 *   nm -g --defined-only build/c6-n4/esp-idf/mbedtls/mbedtls/library/libmbedcrypto.a \
 *     | grep -E ' T (psa_crypto_init|psa_generate_random)$'
 */

#include "od_hal_crypto.h"
#include "od_log.h"

#include "psa/crypto.h"

/* PSA's init brings up the key store and driver dispatch. Cached, like every other od_hal_crypto
 * entry point's backend hook: a per-draw init would pay that cost on every challenge. */
static bool s_psa_ready;

static enum od_hal_crypto_status psa_init_once(void)
{
    psa_status_t status;

    if (s_psa_ready) {
        return OD_HAL_CRYPTO_OK;
    }
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        od_log_error("PSA init failed: %d", (int)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    s_psa_ready = true;
    return OD_HAL_CRYPTO_OK;
}

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *buf, uint16_t len)
{
    psa_status_t status;

    if (buf == NULL && len != 0u) {
        return OD_HAL_CRYPTO_ERROR;
    }
    if (psa_init_once() != OD_HAL_CRYPTO_OK) {
        return OD_HAL_CRYPTO_ERROR;
    }
    status = psa_generate_random(buf, len);
    if (status != PSA_SUCCESS) {
        od_log_error("PSA random failed: %d", (int)status);
        return OD_HAL_CRYPTO_ERROR;
    }
    return OD_HAL_CRYPTO_OK;
}
