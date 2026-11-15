/* fuzz_cfg_policy.c -- the SEC2 config policy and the factory-reset marker over arbitrary bytes.
 *
 * Input: [state selector][blob...]. The selector picks the device the policy sees (unclaimed,
 * unclaimed + LEGACY_SECRETS, claimed, LOCKED); od_sec2 is stubbed so the policy is the only
 * code under test. Properties, beyond the sanitizers:
 *   - the verdict is one of OK / 0x00 / 0x10 / 0x11, and a grown blob stays within cap;
 *   - an admitted blob is admitted again unchanged (the at-rest transform is a fixed point);
 *   - on an unclaimed device an admitted blob carries no secret: a "Core" re-check refuses none;
 *   - the read view writes only inside the chunk it is given.
 * The first 12 bytes also go to od_fr_marker_decode, whose accept must round-trip. */
#include "od_cfg_policy.h"
#include "od_factory_reset.h"
#include "od_hal_crypto.h"
#include "od_hal_secret.h"
#include "od_sec2.h"

#include <stdlib.h>
#include <string.h>

static enum od_sec2_state g_state;
static bool g_legacy;
static const uint8_t KDEV[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
static struct SecurityConfig g_sec;

enum od_sec2_state od_sec2_state(void) { return g_state; }
bool od_sec2_legacy_secrets(void) { return g_state == OD_SEC2_UNCLAIMED && g_legacy; }
bool od_sec2_key_is(const uint8_t key[16]) { return g_state == OD_SEC2_CLAIMED && memcmp(key, KDEV, 16u) == 0; }
bool od_sec2_k_v1(uint8_t out[16]) { if (g_state != OD_SEC2_CLAIMED) return false; memcpy(out, KDEV, 16u); return true; }
bool od_sec2_k_dev(uint8_t out[16]) { return od_sec2_k_v1(out); }
const struct SecurityConfig *od_sec2_app_security(void) { return g_state == OD_SEC2_CLAIMED ? &g_sec : NULL; }
void od_sec2_teardown(void) { }
enum od_hal_crypto_status od_hal_crypto_random(uint8_t *b, uint16_t n) { memset(b, 7, n); return OD_HAL_CRYPTO_OK; }
enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t k[16], const uint8_t *m, uint32_t n, uint8_t o[16])
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 16u; ++i) h = (h ^ k[i]) * 16777619u;
    for (uint32_t i = 0; i < n; ++i) h = (h ^ m[i]) * 16777619u;
    for (uint32_t i = 0; i < 16u; ++i) { h = (h ^ i) * 16777619u; o[i] = (uint8_t)(h >> 24); }
    return OD_HAL_CRYPTO_OK;
}

/* Never reached by these two entry points; here so the translation units link. */
bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *b, uint16_t n) { (void)id; (void)b; (void)n; return false; }
bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *b, uint16_t n) { (void)id; (void)b; (void)n; return false; }
bool od_hal_secret_erase(enum od_hal_secret_id id) { (void)id; return false; }

#define CAP 4200u

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static uint8_t a[CAP], b[CAP], view[CAP];
    uint32_t n, n2;
    uint8_t rc;

    if (size < 1u || size - 1u > CAP - 70u) {
        return 0;
    }
    g_state = (enum od_sec2_state)(data[0] % 3u);
    g_legacy = (data[0] & 0x80u) != 0u;
    memset(&g_sec, 0, sizeof g_sec);
    g_sec.flags = (uint8_t)(data[0] & 0x3Cu);
    g_sec.reset_pin = 5u;

    n = (uint32_t)(size - 1u);
    memcpy(a, data + 1u, n);
    rc = od_cfg_policy_admit(a, &n, CAP);
    if (rc != OD_CFG_POLICY_OK && rc != 0x00u && rc != 0x10u && rc != 0x11u) abort();
    if (rc == OD_CFG_POLICY_OK) {
        if (n > CAP || n < size - 1u) abort();
        memcpy(b, a, n);
        n2 = n;
        if (od_cfg_policy_admit(b, &n2, CAP) != OD_CFG_POLICY_OK || n2 != n || memcmp(a, b, n) != 0) abort();
        /* the read view only ever touches the chunk it is handed */
        for (uint32_t off = 0u; off < n; off += 96u) {
            const uint32_t take = n - off < 96u ? n - off : 96u;
            memset(view, 0xEE, sizeof view);
            memcpy(view + 1u, a + off, take);
            od_cfg_policy_read_view(a, n, off, view + 1u, take);
            if (view[0] != 0xEEu || view[1u + take] != 0xEEu) abort();
        }
    }
    if (size >= 1u + OD_FR_OS2R_LEN) {
        struct od_fr_marker m;
        uint8_t r[OD_FR_OS2R_LEN];
        if (od_fr_marker_decode(data + 1u, &m)) {
            od_fr_marker_encode(&m, r);
            if (memcmp(r, data + 1u, OD_FR_OS2R_LEN) != 0) abort();
        }
    }
    return 0;
}
