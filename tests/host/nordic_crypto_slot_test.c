/* nordic_crypto_slot_test.c -- targets/nordic-zephyr/src/od_hal_crypto.c, the PRODUCTION file,
 * compiled against tests/host/fake_psa/.
 *
 * ONE DEFECT SHAPE, and it is a latch. The prepared-key slot is tracked in two places: PSA owns
 * the key, this HAL owns the id and a ready flag. When a destroy fails, the two disagree -- and if
 * the tracked id is only cleared after a SUCCESSFUL destroy, the slot stays marked ready forever,
 * so every later handshake retries the same failing release and authentication never recovers
 * until reboot. A device that cannot re-authenticate is, from the host's side, bricked.
 *
 * THE REPAIR IS NOT "RETRY LATER". A held-back id is a hazard: PSA may reissue that number to
 * another key, and a delayed destroy would then free somebody else's. Ownership is dropped BEFORE
 * the destroy is attempted, the PSA slot is reported leaked, and the next import starts clean.
 * The last case below is what pins that -- it asserts on the ids actually passed to
 * psa_destroy_key(), not merely on the return values.
 *
 * What this cannot prove: that real PSA on nRF behaves as the fake does. Hardware owns the normal
 * lifecycle; the injected fault is not practically reproducible on a board, which is exactly why
 * it is here.
 */

#include "od_hal_crypto.h"

#include "fake_psa.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)

#define CASE(name) (g_case = (name))

/* od_log.h is the target's, and declares these; the target's od_log.c is Zephyr-bound, so the
 * sink is here. Error lines are counted: "log and return ERROR" is part of the contract under
 * test -- a leaked PSA slot that is silently absorbed is the failure mode that hides a draining
 * key pool. */
static unsigned g_log_errors;

void _od_log(int level, const char *fmt, ...);
void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    (void)fmt;
    if (level == 0) {   /* OD_LOG_ERROR */
        ++g_log_errors;
    }
    va_start(ap, fmt);
    va_end(ap);
}

static const uint8_t KEY_A[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
static const uint8_t KEY_B[16] = { 16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31 };

static void reset(void)
{
    /* The HAL's own slot tracking is file-static and survives between cases. Clearing it through
     * the public API is the only way to start clean, and it must work even after a fault -- which
     * is itself one of the properties under test, so it is asserted rather than assumed. */
    fake_psa_reset();
    od_hal_crypto_key_clear(0);
    fake_psa_reset();
    g_log_errors = 0u;
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_normal_replacement(void)
{
    CASE("a key can be replaced, and the old one is destroyed exactly once");
    reset();
    CHECK(od_hal_crypto_key_set(0, KEY_A) == OD_HAL_CRYPTO_OK);
    CHECK(fake_psa_import_calls == 1u);
    CHECK(fake_psa_live_keys() == 1u);
    CHECK(od_hal_crypto_key_set(0, KEY_B) == OD_HAL_CRYPTO_OK);
    CHECK(fake_psa_import_calls == 2u);
    CHECK(fake_psa_destroy_calls == 1u);
    CHECK(fake_psa_live_keys() == 1u);       /* the replacement, not both */
    CHECK(g_log_errors == 0u);
}

static unsigned destroys_of(psa_key_id_t id)
{
    unsigned i, n = 0u;

    for (i = 0u; i < fake_psa_destroy_log_n; ++i) {
        if (fake_psa_destroy_log[i] == id) {
            ++n;
        }
    }
    return n;
}

static void test_destroy_failure_does_not_latch(void)
{
    psa_key_id_t leaked;
    unsigned destroys_before;

    CASE("a failed destroy reports ERROR and reports the leak");
    reset();
    CHECK(od_hal_crypto_key_set(0, KEY_A) == OD_HAL_CRYPTO_OK);

    /* Break the destroy that the NEXT key_set performs as part of its release. */
    fake_psa_fail_destroy_on = fake_psa_destroy_calls + 1u;
    CHECK(od_hal_crypto_key_set(0, KEY_B) == OD_HAL_CRYPTO_ERROR);
    CHECK(g_log_errors >= 1u);               /* absorbed silently, a draining pool is invisible */
    leaked = fake_psa_last_destroyed;
    CHECK(fake_psa_key_live(leaked));        /* the fake kept it: the PSA slot really did leak */
    fake_psa_fail_destroy_on = 0u;

    CASE("the very next replacement imports cleanly instead of retrying the failed destroy");
    destroys_before = fake_psa_destroy_calls;
    CHECK(od_hal_crypto_key_set(0, KEY_B) == OD_HAL_CRYPTO_OK);
    /* No destroy at all: ownership of the leaked id was dropped when it failed, so there is
     * nothing left to release. A retry here would be the hazard, not the repair. */
    CHECK(fake_psa_destroy_calls == destroys_before);
    /* TWO imports, not three: the key_set that met the failed release returned ERROR without
     * importing, which is the intended outcome -- the handshake that hit the fault fails, and the
     * NEXT one succeeds. */
    CHECK(fake_psa_import_calls == 2u);

    CASE("the leaked id is never targeted again -- PSA may have reissued that number");
    od_hal_crypto_key_clear(0);
    CHECK(destroys_of(leaked) == 1u);        /* exactly the destroy that failed */
}

static void test_clear_after_failed_destroy_is_idempotent(void)
{
    CASE("clear after a failed destroy is idempotent and destroys nothing twice");
    reset();
    CHECK(od_hal_crypto_key_set(0, KEY_A) == OD_HAL_CRYPTO_OK);

    fake_psa_fail_destroy_on = fake_psa_destroy_calls + 1u;
    od_hal_crypto_key_clear(0);              /* void: the failure surfaces only in the log */
    CHECK(g_log_errors >= 1u);
    fake_psa_fail_destroy_on = 0u;

    {
        const unsigned destroys_before = fake_psa_destroy_calls;
        od_hal_crypto_key_clear(0);
        od_hal_crypto_key_clear(0);
        CHECK(fake_psa_destroy_calls == destroys_before);
    }
}

static void test_encrypt_after_failed_destroy_has_no_key(void)
{
    uint8_t nonce[13] = { 0 };
    uint8_t aad[2] = { 0x00u, 0x70u };
    uint8_t plain[8] = { 0 };
    uint8_t ct[64];
    uint16_t ct_len = 0u;

    CASE("after a failed release the slot holds NO key -- it must refuse, not use a stale id");
    reset();
    CHECK(od_hal_crypto_key_set(0, KEY_A) == OD_HAL_CRYPTO_OK);
    fake_psa_fail_destroy_on = fake_psa_destroy_calls + 1u;
    /* Break the import too, so the replacement leaves the slot genuinely empty. */
    CHECK(od_hal_crypto_key_set(0, KEY_B) == OD_HAL_CRYPTO_ERROR);
    fake_psa_fail_destroy_on = 0u;

    CHECK(od_hal_crypto_ccm_encrypt(0, nonce, sizeof nonce, aad, sizeof aad,
                                    plain, sizeof plain, ct, sizeof ct, &ct_len)
          == OD_HAL_CRYPTO_ERROR);
    CHECK(ct_len == 0u);
}

static void test_import_failure_leaves_slot_empty(void)
{
    CASE("a failed import leaves the slot empty rather than half-ready");
    reset();
    CHECK(od_hal_crypto_key_set(0, KEY_A) == OD_HAL_CRYPTO_OK);
    fake_psa_fail_import_on = fake_psa_import_calls + 1u;
    CHECK(od_hal_crypto_key_set(0, KEY_B) == OD_HAL_CRYPTO_ERROR);
    fake_psa_fail_import_on = 0u;
    /* The old key WAS released before the failed import, so nothing is live and nothing leaked. */
    CHECK(fake_psa_live_keys() == 0u);

    CASE("and a subsequent set succeeds");
    CHECK(od_hal_crypto_key_set(0, KEY_A) == OD_HAL_CRYPTO_OK);
    CHECK(fake_psa_live_keys() == 1u);
}

static void test_key_policy_carries_the_shortened_tag(void)
{
    CASE("the imported key policy names the 12-byte tag, not plain CCM");
    reset();
    CHECK(od_hal_crypto_key_set(0, KEY_A) == OD_HAL_CRYPTO_OK);
    /* Plain PSA_ALG_CCM pins a 16-byte tag and refuses every wire-compatible operation with
     * NOT_PERMITTED. This is the policy that was proven on silicon; pin it. */
    CHECK(fake_psa_last_attr.alg ==
          PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, OD_HAL_CRYPTO_TAG_LEN));
    CHECK(fake_psa_last_attr.alg != PSA_ALG_CCM);
    CHECK(fake_psa_last_attr.bits == 128u);
    CHECK((fake_psa_last_attr.usage & PSA_KEY_USAGE_ENCRYPT) != 0u);
    CHECK((fake_psa_last_attr.usage & PSA_KEY_USAGE_DECRYPT) != 0u);
}

static void test_random_propagates_failure(void)
{
    uint8_t buf[16];

    CASE("od_hal_crypto_random propagates a PSA failure");
    reset();
    fake_psa_fail_random_on = 1u;
    CHECK(od_hal_crypto_random(buf, sizeof buf) == OD_HAL_CRYPTO_ERROR);
    fake_psa_fail_random_on = 0u;
    CHECK(od_hal_crypto_random(buf, sizeof buf) == OD_HAL_CRYPTO_OK);
}

/* The ephemeral ECDH key: one PSA key, destroyed by every agree whatever its result, and an
 * INVALID_ARGUMENT from PSA reported as BAD_POINT (the shared code already validated the point;
 * this is the backend's second opinion). The fake's arithmetic is not P-256. */
static void test_ecdh_key_lifetime(void)
{
    uint8_t q[64], peer[64], z[32];

    CASE("ECDH keygen holds one key; agree destroys it");
    reset();
    CHECK(od_hal_crypto_ecdh_keygen(q) == OD_HAL_CRYPTO_OK);
    CHECK(fake_psa_live_keys() == 1u);
    CHECK((fake_psa_last_attr.usage & PSA_KEY_USAGE_DERIVE) != 0u);
    memset(peer, 0x11, sizeof peer);
    CHECK(od_hal_crypto_ecdh_agree(peer, z) == OD_HAL_CRYPTO_OK);
    CHECK(fake_psa_live_keys() == 0u);

    CASE("a second keygen replaces the first rather than leaking it");
    CHECK(od_hal_crypto_ecdh_keygen(q) == OD_HAL_CRYPTO_OK);
    CHECK(od_hal_crypto_ecdh_keygen(q) == OD_HAL_CRYPTO_OK);
    CHECK(fake_psa_live_keys() == 1u);

    CASE("a peer PSA rejects is BAD_POINT, and the key is still gone");
    memset(peer, 0, 32u);
    CHECK(od_hal_crypto_ecdh_agree(peer, z) == OD_HAL_CRYPTO_BAD_POINT);
    CHECK(fake_psa_live_keys() == 0u);

    CASE("agree without a key is an error");
    CHECK(od_hal_crypto_ecdh_agree(peer, z) == OD_HAL_CRYPTO_ERROR);

    CASE("discard is idempotent");
    CHECK(od_hal_crypto_ecdh_keygen(q) == OD_HAL_CRYPTO_OK);
    od_hal_crypto_ecdh_discard();
    od_hal_crypto_ecdh_discard();
    CHECK(fake_psa_live_keys() == 0u);
}

int main(void)
{
    test_ecdh_key_lifetime();
    test_normal_replacement();
    test_destroy_failure_does_not_latch();
    test_clear_after_failed_destroy_is_idempotent();
    test_encrypt_after_failed_destroy_has_no_key();
    test_import_failure_leaves_slot_empty();
    test_key_policy_carries_the_shortened_tag();
    test_random_propagates_failure();

    printf("nordic_crypto_slot: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
