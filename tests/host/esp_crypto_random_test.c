/* esp_crypto_random_test.c -- targets/esp32-idf/hal/od_hal_crypto_random.c, the PRODUCTION file,
 * compiled against tests/host/fake_psa/.
 *
 * THE CONTRACT IS FALLIBILITY. od_hal_crypto_random() feeds the SEC2 handshake nonces, and
 * od_sec2's rule is that a device must never send a nonce it cannot vouch for: one built from a
 * failed draw is either predictable or stale, and the session derived from it is worthless. That
 * rule is unenforceable while the backend cannot report failure -- which is the state
 * esp_fill_random() leaves it in, since it returns void.
 *
 * NO SILENT FALLBACK. A PSA error must not quietly drop back to esp_fill_random(): the caller
 * would then get a "success" indistinguishable from a real one, which is the exact behaviour this
 * suite exists to remove.
 *
 * ORDER MATTERS IN main(). The adapter initialises its backend once and caches that, as every
 * od_hal_crypto entry point does, so the init-failure case can only be reached before a successful
 * init has been cached. It runs first, and says so.
 */

#include "od_hal_crypto.h"

#include "fake_psa.h"

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

/* ----------------------------------------------------------------------------------- cases --- */

/* FIRST, and only once: see the header comment. */
static void test_init_failure_propagates(void)
{
    uint8_t buf[16];

    CASE("a backend init failure is reported, and no bytes are claimed");
    fake_psa_reset();
    fake_psa_fail_init_on = 1u;
    memset(buf, 0, sizeof buf);
    CHECK(od_hal_crypto_random(buf, sizeof buf) == OD_HAL_CRYPTO_ERROR);
    CHECK(fake_psa_random_calls == 0u);      /* it did not draw against an uninitialised engine */
    fake_psa_fail_init_on = 0u;
}

static void test_success(void)
{
    uint8_t buf[16];

    CASE("a successful draw reports OK and fills the buffer");
    fake_psa_reset();
    memset(buf, 0, sizeof buf);
    CHECK(od_hal_crypto_random(buf, sizeof buf) == OD_HAL_CRYPTO_OK);
    CHECK(fake_psa_random_calls == 1u);
    {
        unsigned i, nonzero = 0u;
        for (i = 0u; i < sizeof buf; ++i) {
            if (buf[i] != 0u) {
                ++nonzero;
            }
        }
        CHECK(nonzero == sizeof buf);
    }
}

static void test_generation_failure_propagates(void)
{
    uint8_t buf[16];

    CASE("a generation failure is reported rather than absorbed");
    fake_psa_reset();
    fake_psa_fail_random_on = 1u;
    CHECK(od_hal_crypto_random(buf, sizeof buf) == OD_HAL_CRYPTO_ERROR);
    CHECK(fake_psa_random_calls == 1u);

    CASE("and there is no fallback draw behind it");
    /* One call, not two. A fallback would show up as a second attempt after the injected failure
     * -- and would hand the caller bytes it has just been told it could not have. */
    CHECK(fake_psa_random_calls == 1u);
    fake_psa_fail_random_on = 0u;

    CASE("the next draw still succeeds -- a failure does not latch the adapter shut");
    CHECK(od_hal_crypto_random(buf, sizeof buf) == OD_HAL_CRYPTO_OK);
}

static void test_null_and_zero(void)
{
    uint8_t buf[4];

    CASE("NULL with a non-zero length is refused before the backend is touched");
    fake_psa_reset();
    CHECK(od_hal_crypto_random(NULL, 4u) == OD_HAL_CRYPTO_ERROR);
    CHECK(fake_psa_random_calls == 0u);

    CASE("a zero-length draw is not an error");
    CHECK(od_hal_crypto_random(buf, 0u) == OD_HAL_CRYPTO_OK);
}

int main(void)
{
    test_init_failure_propagates();      /* must be first: the backend init is cached */
    test_success();
    test_generation_failure_propagates();
    test_null_and_zero();

    printf("esp_crypto_random: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
