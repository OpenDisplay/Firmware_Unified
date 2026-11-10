/* net_a_test.c -- the network A advertisement is a SPECIFICATION, so it is pinned here.
 *
 * Every byte of the record and every byte of the derived address is defined by the finding
 * network, not by us. Getting one wrong does not fail: the device advertises happily, the
 * stack reports success, and the tag is simply never found. There is no error path to observe
 * and no hardware test short of a real finder network, which is exactly why the layout is
 * asserted against literals here rather than against the code that produces it.
 *
 * The two bits of key[0] displaced by the static-random marker are the subtle part. They are
 * not discarded -- they reappear as the status byte at adv[29], and a finder needs both halves
 * to rebuild the key. A "simplification" that dropped either is undetectable from the device.
 */
#include "od_net_a.h"
#include "od_net_g.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "od_hal_adv2.h"
#include "od_hal_log.h"
#include "od_hal_time.h"

/* ------------------------------------------------------------------------- fake adv2 HAL --- */
/* Records what the policy asked the stack to do, so the sync half is testable without one. */
static struct {
    bool     supported;
    unsigned programs, starts, stops;
    uint8_t  adv[OD_NET_A_LEGACY_ADV_LEN];
    uint8_t  adv_len;
    uint8_t  addr[6];
    uint16_t interval;
    uint8_t  program_set, start_set, stop_set;   /* last set index seen per call */
    enum od_hal_adv_result program_result, start_result;
} g_hal;

static void hal_reset(void)
{
    memset(&g_hal, 0, sizeof g_hal);
    g_hal.supported = true;
    g_hal.program_result = OD_HAL_ADV_OK;
    g_hal.start_result = OD_HAL_ADV_OK;
    g_hal.program_set = g_hal.start_set = g_hal.stop_set = 0xFFu;
}

bool od_hal_adv2_supported(void) { return g_hal.supported; }

enum od_hal_adv_result od_hal_adv2_program(uint8_t set, const uint8_t *adv, uint8_t adv_len,
                                           const uint8_t addr[6], uint16_t interval_slots)
{
    g_hal.programs++;
    g_hal.program_set = set;
    if (adv_len <= sizeof g_hal.adv) {
        memcpy(g_hal.adv, adv, adv_len);
    }
    g_hal.adv_len = adv_len;
    memcpy(g_hal.addr, addr, 6u);
    g_hal.interval = interval_slots;
    return g_hal.program_result;
}

enum od_hal_adv_result od_hal_adv2_start(uint8_t set)
{
    g_hal.starts++;
    g_hal.start_set = set;
    return g_hal.start_result;
}

enum od_hal_adv_result od_hal_adv2_stop(uint8_t set)
{
    g_hal.stops++;
    g_hal.stop_set = set;
    return OD_HAL_ADV_OK;
}

/* ------------------------------------------------------------------------- fake log HAL --- */
/* od_net_a logs, which pulls od_log out of od_shared, which needs these. Discarded rather
 * than captured: what this suite asserts is the record and the policy, not the wording. */
void od_hal_log_open(void) { }
bool od_hal_log_is_open(void) { return false; }
uint32_t od_hal_log_cycle_count(void) { return 0u; }
void od_hal_log_flush(void) { }
uint32_t od_hal_uptime_ms(void) { return 0u; }
void od_hal_delay_us(uint32_t us) { (void)us; }
void od_hal_log_write(char *record, size_t len) { (void)record; (void)len; }

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);  \
        }                                                                       \
    } while (0)

#define CASE(name) (g_case = (name))

/* key[0] = 0x9B is chosen so both halves of the split are non-trivial:
 *   0x9B | 0xC0 = 0xDB  (marker set, low six bits preserved)
 *   0x9B >> 6   = 0x02  (a distinguishable status byte, not 0)
 * A key starting 0x00 or 0xFF would pass even if one half were dropped. */
static void fill_key(uint8_t key[OD_NET_A_ADV_KEY_LEN])
{
    key[0] = 0x9Bu;
    for (unsigned i = 1u; i < OD_NET_A_ADV_KEY_LEN; ++i) {
        key[i] = (uint8_t)(0x10u + i);      /* 0x11, 0x12, ... distinct and checkable */
    }
}

static void test_config_active(void)
{
    struct FindMyConfig cfg;

    CASE("NULL is not active");
    CHECK(od_net_a_config_active(NULL) == false);

    CASE("the enable flag alone is not enough -- a zero key must not advertise");
    memset(&cfg, 0, sizeof cfg);
    cfg.flags = OD_FINDMY_FLAG_NET_A_ENABLE;
    /* This is the shape an enabled-but-unprovisioned device has. Advertising it would publish
     * a well-formed record for the all-zero identity, which is worse than staying dark. */
    CHECK(od_net_a_config_active(&cfg) == false);

    CASE("a key alone is not enough -- the flag gates it");
    memset(&cfg, 0, sizeof cfg);
    fill_key(cfg.net_a_adv_key);
    CHECK(od_net_a_config_active(&cfg) == false);

    CASE("flag plus non-zero key is active");
    cfg.flags = OD_FINDMY_FLAG_NET_A_ENABLE;
    CHECK(od_net_a_config_active(&cfg) == true);

    CASE("the FMDN flag does not enable network A");
    memset(&cfg, 0, sizeof cfg);
    fill_key(cfg.net_a_adv_key);
    cfg.flags = OD_FINDMY_FLAG_FMDN_ENABLE;
    CHECK(od_net_a_config_active(&cfg) == false);

    CASE("a single non-zero byte anywhere in the key counts");
    memset(&cfg, 0, sizeof cfg);
    cfg.flags = OD_FINDMY_FLAG_NET_A_ENABLE;
    cfg.net_a_adv_key[OD_NET_A_ADV_KEY_LEN - 1u] = 0x01u;
    CHECK(od_net_a_config_active(&cfg) == true);
}

static void test_derive_address(void)
{
    uint8_t key[OD_NET_A_ADV_KEY_LEN];
    uint8_t addr[6];

    fill_key(key);
    od_net_a_derive_address(key, addr);

    CASE("the address is the top six key bytes, most significant first");
    /* addr is little-endian: addr[5] is the MSB. */
    CHECK(addr[5] == 0xDBu);          /* key[0] | 0xC0 */
    CHECK(addr[4] == key[1]);
    CHECK(addr[3] == key[2]);
    CHECK(addr[2] == key[3]);
    CHECK(addr[1] == key[4]);
    CHECK(addr[0] == key[5]);

    CASE("the two top bits are forced to 0b11 -- a static random address");
    CHECK((addr[5] & 0xC0u) == 0xC0u);

    CASE("the low six bits of key[0] survive into the address");
    CHECK((addr[5] & 0x3Fu) == (key[0] & 0x3Fu));

    CASE("a zero key still yields the marker, nothing else");
    memset(key, 0, sizeof key);
    od_net_a_derive_address(key, addr);
    CHECK(addr[5] == 0xC0u);
    for (unsigned i = 0u; i < 5u; ++i) {
        CHECK(addr[i] == 0x00u);
    }
}

static void test_build_adv(void)
{
    uint8_t key[OD_NET_A_ADV_KEY_LEN];
    uint8_t adv[OD_NET_A_LEGACY_ADV_LEN];
    uint8_t len;

    fill_key(key);
    memset(adv, 0xAA, sizeof adv);
    len = od_net_a_build_adv(key, adv, (uint8_t)sizeof adv);

    CASE("the record is exactly 31 bytes");
    CHECK(len == OD_NET_A_LEGACY_ADV_LEN);
    CHECK(len == 31u);

    CASE("the AD header is the network's, byte for byte");
    CHECK(adv[0] == 0x1Eu);     /* length: 30 bytes follow */
    CHECK(adv[1] == 0xFFu);     /* manufacturer specific data */
    CHECK(adv[2] == 0x4Cu);     /* company id, little-endian */
    CHECK(adv[3] == 0x00u);
    CHECK(adv[4] == 0x12u);     /* offline finding type */
    CHECK(adv[5] == 0x19u);     /* payload length: 25 */
    CHECK(adv[6] == 0x00u);     /* status */

    CASE("bytes 6..27 of the key are carried at offset 7");
    /* The first six key bytes are NOT repeated here -- they are in the address. A build that
     * copied from key[0] would look plausible and be unfindable. */
    for (unsigned i = 0u; i < 22u; ++i) {
        CHECK(adv[7u + i] == key[6u + i]);
    }
    CHECK(adv[7] == key[6]);
    CHECK(adv[28] == key[27]);

    CASE("the displaced top bits of key[0] reappear as the status byte");
    CHECK(adv[29] == 0x02u);            /* 0x9B >> 6 */
    CHECK(adv[29] == (uint8_t)(key[0] >> 6));

    CASE("the trailing hint byte is zero");
    CHECK(adv[30] == 0x00u);

    CASE("a buffer too small is refused, not truncated");
    memset(adv, 0xAA, sizeof adv);
    CHECK(od_net_a_build_adv(key, adv, (uint8_t)(OD_NET_A_LEGACY_ADV_LEN - 1u)) == 0u);
    CHECK(adv[0] == 0xAAu);             /* nothing written */

    CASE("a NULL buffer is refused");
    CHECK(od_net_a_build_adv(key, NULL, OD_NET_A_LEGACY_ADV_LEN) == 0u);
}

static void test_address_and_payload_are_complementary(void)
{
    uint8_t key[OD_NET_A_ADV_KEY_LEN];
    uint8_t adv[OD_NET_A_LEGACY_ADV_LEN];
    uint8_t addr[6];

    CASE("every key byte is recoverable from address + payload, for every key[0]");
    /* The whole 28-byte key must be reconstructible by a finder. Sweeping key[0] catches a
     * mask or shift that happens to work for one value. */
    for (unsigned k0 = 0u; k0 < 256u; ++k0) {
        fill_key(key);
        key[0] = (uint8_t)k0;
        od_net_a_derive_address(key, addr);
        (void)od_net_a_build_adv(key, adv, (uint8_t)sizeof adv);

        /* key[0] = low six bits from the address, top two from the status byte. */
        const uint8_t rebuilt0 = (uint8_t)((addr[5] & 0x3Fu) | (uint8_t)(adv[29] << 6));
        CHECK(rebuilt0 == key[0]);
    }

    CASE("key bytes 1..5 come back from the address");
    fill_key(key);
    od_net_a_derive_address(key, addr);
    CHECK(addr[4] == key[1]);
    CHECK(addr[0] == key[5]);
}

static void active_cfg(struct FindMyConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->flags = OD_FINDMY_FLAG_NET_A_ENABLE;
    fill_key(cfg->net_a_adv_key);
}

static void test_sync_policy(void)
{
    struct FindMyConfig cfg;

    CASE("an active config programs and starts, once");
    hal_reset();
    od_net_a_invalidate();
    active_cfg(&cfg);
    od_net_a_sync(&cfg);
    CHECK(g_hal.programs == 1u);
    CHECK(g_hal.starts == 1u);
    CHECK(g_hal.adv_len == OD_NET_A_LEGACY_ADV_LEN);
    CHECK(g_hal.interval == OD_NET_A_ADV_INTERVAL_SLOTS);
    CHECK(g_hal.addr[5] == 0xDBu);
    CHECK(g_hal.program_set == OD_HAL_ADV2_SET_NET_A);
    CHECK(g_hal.start_set == OD_HAL_ADV2_SET_NET_A);

    CASE("syncing again is a no-op -- a finding advertiser must not be restarted every pass");
    od_net_a_sync(&cfg);
    od_net_a_sync(&cfg);
    CHECK(g_hal.programs == 1u);
    CHECK(g_hal.starts == 1u);

    CASE("an inactive config stops it");
    cfg.flags = 0u;
    od_net_a_sync(&cfg);
    CHECK(g_hal.stops >= 1u);

    CASE("stopping twice does not re-stop");
    const unsigned stops_after_first = g_hal.stops;
    od_net_a_sync(&cfg);
    CHECK(g_hal.stops == stops_after_first);

    CASE("NULL config is treated as inactive, not as a crash");
    hal_reset();
    od_net_a_invalidate();
    od_net_a_sync(NULL);
    CHECK(g_hal.programs == 0u);
    CHECK(g_hal.starts == 0u);

    CASE("a build with no second advertising set does nothing at all");
    hal_reset();
    od_net_a_invalidate();
    g_hal.supported = false;
    active_cfg(&cfg);
    od_net_a_sync(&cfg);
    CHECK(g_hal.programs == 0u);
    CHECK(g_hal.starts == 0u);
    CHECK(g_hal.stops == 0u);

    CASE("invalidate forces a reprogram -- the key, and so the address, may have changed");
    hal_reset();
    od_net_a_invalidate();
    active_cfg(&cfg);
    od_net_a_sync(&cfg);
    CHECK(g_hal.programs == 1u);
    cfg.net_a_adv_key[0] = 0x11u;          /* new key -> different derived address */
    od_net_a_invalidate();
    od_net_a_sync(&cfg);
    CHECK(g_hal.programs == 2u);
    /* Without the invalidate the old key would still be advertising while config says
     * otherwise -- which no error path would ever report. */
    CHECK(g_hal.addr[5] == (uint8_t)(0x11u | 0xC0u));

    CASE("a failed program leaves it unstarted, and the next pass retries");
    hal_reset();
    od_net_a_invalidate();
    active_cfg(&cfg);
    g_hal.program_result = OD_HAL_ADV_RETRY;
    od_net_a_sync(&cfg);
    CHECK(g_hal.starts == 0u);
    g_hal.program_result = OD_HAL_ADV_OK;
    od_net_a_sync(&cfg);
    CHECK(g_hal.starts == 1u);

    CASE("a failed start also retries rather than latching");
    hal_reset();
    od_net_a_invalidate();
    active_cfg(&cfg);
    g_hal.start_result = OD_HAL_ADV_ERROR;
    od_net_a_sync(&cfg);
    CHECK(g_hal.starts == 1u);
    g_hal.start_result = OD_HAL_ADV_OK;
    od_net_a_sync(&cfg);
    CHECK(g_hal.starts == 2u);
}

/* ------------------------------------------------------------------- network G (FMDN) --- */
/* The expected bytes are transcribed from Firmware_Silabs 2.3.0 (origin/main fd3bae8)
 * opendisplay_ble.c od_fmdn_build_and_apply_adv, SECP160R1 branch, lines 2062-2072, with the
 * hashed-flags byte as 2.3.0 actually emits it (0x00: its PSA build has no SHA-256, so
 * od_fmdn_hashed_flags_secp160 returns 0 on every unit). Address: od_fmdn_apply_address_from_key
 * lines 2011-2016. */
static void fill_eid(uint8_t eid[OD_NET_G_EID_LEN])
{
    eid[0] = 0x9Bu;
    for (unsigned i = 1u; i < OD_NET_G_EID_LEN; ++i) {
        eid[i] = (uint8_t)(0xA0u + i);
    }
}

static void g_active_cfg(struct FindMyConfig *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->flags = OD_FINDMY_FLAG_FMDN_ENABLE;
    cfg->fmdn_k = 10u;
    cfg->fmdn_curve = OD_FINDMY_CURVE_SECP160R1;
    fill_eid(cfg->advertisement_key);
}

static void test_net_g_vector(void)
{
    static const uint8_t want[OD_NET_G_LEGACY_ADV_LEN] = {
        0x02, 0x01, 0x06, 0x19, 0x16, 0xAA, 0xFE, 0x41,
        0x9B, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9,
        0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3,
        0x00,
    };
    uint8_t eid[OD_NET_G_EID_LEN];
    uint8_t adv[OD_NET_G_LEGACY_ADV_LEN + 2u];
    uint8_t addr[6];

    fill_eid(eid);
    memset(adv, 0x55, sizeof adv);

    CASE("net_g: the 29-byte record matches 2.3.0 byte for byte");
    CHECK(od_net_g_build_adv(eid, adv, (uint8_t)sizeof adv) == 29u);
    CHECK(memcmp(adv, want, sizeof want) == 0);
    CHECK(adv[29] == 0x55u);              /* nothing past the record */
    /* AD length byte covers type + UUID + frame + EID + flags. */
    CHECK(adv[3] == (uint8_t)(OD_NET_G_LEGACY_ADV_LEN - 4u));

    CASE("net_g: a short buffer is refused, not truncated");
    memset(adv, 0x55, sizeof adv);
    CHECK(od_net_g_build_adv(eid, adv, (uint8_t)(OD_NET_G_LEGACY_ADV_LEN - 1u)) == 0u);
    CHECK(adv[0] == 0x55u);
    CHECK(od_net_g_build_adv(eid, NULL, 64u) == 0u);

    CASE("net_g: address is eid[0..5] MSB first with the static-random marker");
    od_net_g_derive_address(eid, addr);
    CHECK(addr[5] == 0xDBu);
    CHECK(addr[4] == 0xA1u);
    CHECK(addr[3] == 0xA2u);
    CHECK(addr[2] == 0xA3u);
    CHECK(addr[1] == 0xA4u);
    CHECK(addr[0] == 0xA5u);
}

static void test_net_g_active(void)
{
    struct FindMyConfig cfg;

    CASE("net_g: NULL is not active");
    CHECK(!od_net_g_config_active(NULL));

    CASE("net_g: flag + SECP160 + non-zero advertisement_key is active");
    g_active_cfg(&cfg);
    CHECK(od_net_g_config_active(&cfg));

    CASE("net_g: the network-A flag alone does not enable G");
    cfg.flags = OD_FINDMY_FLAG_NET_A_ENABLE;
    CHECK(!od_net_g_config_active(&cfg));

    CASE("net_g: zero advertisement_key is not active, whatever the EIK");
    g_active_cfg(&cfg);
    memset(cfg.advertisement_key, 0, sizeof cfg.advertisement_key);
    memset(cfg.eik, 0x77, sizeof cfg.eik);
    CHECK(!od_net_g_config_active(&cfg));

    CASE("net_g: the last key byte alone counts");
    cfg.advertisement_key[OD_NET_G_EID_LEN - 1u] = 1u;
    CHECK(od_net_g_config_active(&cfg));

    CASE("net_g: SECP256R1 never advertises (2.3.0 could not compute its EID either)");
    g_active_cfg(&cfg);
    cfg.fmdn_curve = OD_FINDMY_CURVE_SECP256R1;
    cfg.time_base_unix = 1700000000u;
    memset(cfg.eik, 0x77, sizeof cfg.eik);
    CHECK(!od_net_g_config_active(&cfg));

    CASE("net_g: unknown curve is not active");
    g_active_cfg(&cfg);
    cfg.fmdn_curve = 2u;
    CHECK(!od_net_g_config_active(&cfg));
}

static void test_net_g_sync(void)
{
    struct FindMyConfig cfg;

    CASE("net_g: active config programs set G once, legacy 1 s");
    hal_reset();
    od_net_g_invalidate();
    g_active_cfg(&cfg);
    od_net_g_sync(&cfg);
    CHECK(g_hal.programs == 1u && g_hal.starts == 1u);
    CHECK(g_hal.program_set == OD_HAL_ADV2_SET_NET_G);
    CHECK(g_hal.start_set == OD_HAL_ADV2_SET_NET_G);
    CHECK(g_hal.adv_len == OD_NET_G_LEGACY_ADV_LEN);
    CHECK(g_hal.interval == 1600u);
    CHECK(g_hal.addr[5] == 0xDBu);
    od_net_g_sync(&cfg);
    CHECK(g_hal.programs == 1u && g_hal.starts == 1u);

    CASE("net_g: disabling stops set G, never set A");
    cfg.flags = 0u;
    od_net_g_sync(&cfg);
    CHECK(g_hal.stops >= 1u);
    CHECK(g_hal.stop_set == OD_HAL_ADV2_SET_NET_G);

    CASE("net_g: invalidate after a key change reprograms with the new address");
    hal_reset();
    od_net_g_invalidate();
    g_active_cfg(&cfg);
    od_net_g_sync(&cfg);
    cfg.advertisement_key[0] = 0x01u;
    od_net_g_invalidate();
    od_net_g_sync(&cfg);
    CHECK(g_hal.programs == 2u);
    CHECK(g_hal.addr[5] == 0xC1u);
    CHECK(g_hal.adv[8] == 0x01u);

    CASE("net_g: a failed program retries next pass");
    hal_reset();
    od_net_g_invalidate();
    g_active_cfg(&cfg);
    g_hal.program_result = OD_HAL_ADV_RETRY;
    od_net_g_sync(&cfg);
    CHECK(g_hal.starts == 0u);
    g_hal.program_result = OD_HAL_ADV_OK;
    od_net_g_sync(&cfg);
    CHECK(g_hal.starts == 1u);

    CASE("net_g: no adv2 support, no calls");
    hal_reset();
    od_net_g_invalidate();
    g_hal.supported = false;
    od_net_g_sync(&cfg);
    CHECK(g_hal.programs == 0u && g_hal.stops == 0u);
}

int main(void)
{
    test_net_g_vector();
    test_net_g_active();
    test_net_g_sync();
    test_config_active();
    test_derive_address();
    test_build_adv();
    test_address_and_payload_are_complementary();
    test_sync_policy();

    printf("net_a+net_g: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
