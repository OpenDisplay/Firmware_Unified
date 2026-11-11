/* cfg_policy_core_test.c -- a "Core" build without the SEC2 module (OD_SEC2=0, Q14): every blob
 * carrying a secret is refused 0x10 whatever the device state, and a clean one is admitted.
 * od_cfg_policy.c is compiled into this executable with the override. */
#include "od_cfg_policy.h"
#include "od_config_tlv.h"
#include "od_hal_crypto.h"
#include "od_sec2.h"
#include "od_sec2_wire.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks, g_failures;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_failures; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The policy asks od_sec2 nothing in this build; these exist only so the other functions link. */
enum od_sec2_state od_sec2_state(void) { return OD_SEC2_CLAIMED; }
bool od_sec2_legacy_secrets(void) { return true; }
bool od_sec2_key_is(const uint8_t key[16]) { (void)key; return true; }
bool od_sec2_k_v1(uint8_t out[16]) { memset(out, 0, 16u); return true; }
bool od_sec2_k_dev(uint8_t out[16]) { memset(out, 0, 16u); return true; }
const struct SecurityConfig *od_sec2_app_security(void) { return NULL; }

enum od_hal_crypto_status od_hal_crypto_random(uint8_t *b, uint16_t n) { memset(b, 0, n); return OD_HAL_CRYPTO_OK; }
enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t k[16], const uint8_t *m, uint32_t n, uint8_t o[16])
{ (void)k; (void)m; (void)n; memset(o, 0, 16u); return OD_HAL_CRYPTO_OK; }

static uint32_t blob(uint8_t *b, uint8_t id, uint16_t body_n, uint32_t at, uint8_t v)
{
    uint16_t c;

    memset(b, 0, 5u + 2u + body_n);
    b[2] = 1u;
    b[4] = id;
    b[5u + at] = v;
    c = od_config_tlv_crc16(od_span_make(b, 5u + body_n));
    b[5u + body_n] = (uint8_t)c;
    b[6u + body_n] = (uint8_t)(c >> 8);
    return 7u + body_n;
}

int main(void)
{
    uint8_t b[300];
    uint32_t n;

    n = blob(b, 0x26u, 160u, 32u, 1u);
    CHECK(od_cfg_policy_admit(b, &n, sizeof b) == OD_ERR_CONFIG_SECRET_NEEDS_CLAIM);
    n = blob(b, 0x27u, 64u, 5u, 1u);           /* a key byte */
    CHECK(od_cfg_policy_admit(b, &n, sizeof b) == OD_ERR_CONFIG_SECRET_NEEDS_CLAIM);
    n = blob(b, 0x27u, 64u, 0u, 1u);           /* enabled */
    CHECK(od_cfg_policy_admit(b, &n, sizeof b) == OD_ERR_CONFIG_SECRET_NEEDS_CLAIM);
    n = blob(b, 0x2Du, 128u, 80u, 1u);         /* advertisement_key */
    CHECK(od_cfg_policy_admit(b, &n, sizeof b) == OD_ERR_CONFIG_SECRET_NEEDS_CLAIM);
    n = blob(b, 0x26u, 160u, 0u, 'N');         /* an SSID alone is not a secret */
    CHECK(od_cfg_policy_admit(b, &n, sizeof b) == OD_CFG_POLICY_OK);
    printf("cfg_policy_core: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
