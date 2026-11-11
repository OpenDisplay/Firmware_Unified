/* nordic_sec2_app_test.c -- targets/nordic-zephyr/src/od_sec2_app.c, the PRODUCTION file,
 * against fake Zephyr seams.
 *
 * THE UID IS WIRE-VISIBLE. It is the KDF context of every SEC2 key, so which bytes and in what
 * order decides which device a host thinks it is talking to; a change here strands every claimed
 * device. SECURITY_V2 4.3 fixes it as the first eight bytes hwinfo writes, in buffer order. The
 * expectations are hand-written, not taken from the code.
 */

#include "od_sec2_app.h"
#include "od_sec2_wire.h"

#include "od_config.h"
#include "opendisplay_structs.h"
#include "zephyr/drivers/hwinfo.h"
#include "zephyr/kernel.h"

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

/* --------------------------------------------------------------------------------- the fakes --- */

static uint8_t g_hw_id[8];
static size_t  g_hw_len = 8u;
static int     g_hw_rc;              /* negative to make hwinfo fail */

int hwinfo_get_device_id(uint8_t *buffer, size_t length)
{
    size_t n = (g_hw_len < length) ? g_hw_len : length;

    if (g_hw_rc < 0) {
        return g_hw_rc;
    }
    if (buffer != NULL && n != 0u) {
        memcpy(buffer, g_hw_id, n);
    }
    return (int)n;
}

uint32_t fake_k_uptime_ms;
uint32_t k_uptime_get_32(void) { return fake_k_uptime_ms; }

static struct SecurityConfig g_sec_cfg;
static const struct SecurityConfig *g_sec_ptr;
static struct od_config g_cfg;

const struct SecurityConfig *od_get_parsed_security(void) { return g_sec_ptr; }
const struct od_config *opendisplay_get_global_config(void) { return &g_cfg; }

/* ----------------------------------------------------------------------------------- cases --- */

static void test_uid(void)
{
    static const uint8_t ID[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };
    uint8_t uid[8];

    CASE("the uid is hwinfo's eight bytes, in buffer order");
    memcpy(g_hw_id, ID, 8u);
    g_hw_len = 8u;
    g_hw_rc = 0;
    od_sec2_app_uid(uid);
    CHECK(memcmp(uid, ID, 8u) == 0);

    CASE("a driver that writes fewer bytes leaves zeros, not stack residue");
    g_hw_len = 6u;
    memset(uid, 0x5A, sizeof uid);
    od_sec2_app_uid(uid);
    CHECK(memcmp(uid, ID, 6u) == 0 && uid[6] == 0u && uid[7] == 0u);

    CASE("a failing hwinfo yields the zero uid");
    g_hw_rc = -5;
    memset(uid, 0x5A, sizeof uid);
    od_sec2_app_uid(uid);
    CHECK(uid[0] == 0u && uid[3] == 0u && uid[7] == 0u);
    g_hw_rc = 0;
}

static void test_dflags_and_seams(void)
{
    CASE("no config: no flags");
    memset(&g_cfg, 0, sizeof g_cfg);
    g_sec_ptr = NULL;
    CHECK(od_sec2_app_dflags() == 0u);

    CASE("a loaded config with a panel: HAS_PANEL");
    g_cfg.loaded = true;
    g_cfg.display_count = 1u;
    CHECK(od_sec2_app_dflags() == OD_SEC2_DFLAG_HAS_PANEL);

    CASE("the reset-pin flag: HAS_RESET_PIN");
    memset(&g_sec_cfg, 0, sizeof g_sec_cfg);
    g_sec_cfg.flags = OD_SECURITY_FLAG_RESET_PIN_ENABLED;
    g_sec_ptr = &g_sec_cfg;
    CHECK(od_sec2_app_dflags() == (OD_SEC2_DFLAG_HAS_PANEL | OD_SEC2_DFLAG_HAS_RESET_PIN));
    CHECK(od_sec2_app_security() == &g_sec_cfg);

    CASE("the clock is the kernel uptime");
    fake_k_uptime_ms = 0xFFFFFFFFu;
    CHECK(od_sec2_app_now_ms() == 0xFFFFFFFFu);
}

int main(void)
{
    test_uid();
    test_dflags_and_seams();
    printf("nordic_sec2_app: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
