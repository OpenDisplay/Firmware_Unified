/* claim_mode_test.c -- od_security_boot(), the boot history (claim gesture and the five-cycle
 * factory reset), the claim-mode session and the factory reset (pin machine, R1-R8, a power cut
 * at every storage mutation), over the real od_sec2, od_cfg_policy and od_config_store.
 *
 * The device is simulated boot by boot: storage (secrets + config record) and the retained 16 B
 * persist as the reset cause says they would; everything in RAM is forgotten. Time is uptime. */
#include "od_advert.h"
#include "od_cfg_policy.h"
#include "od_claim_app.h"
#include "od_claim_mode.h"
#include "od_config_store.h"
#include "od_config_tlv.h"
#include "od_factory_reset.h"
#include "od_hal_crypto.h"
#include "od_hal_nvs.h"
#include "od_hal_radio.h"
#include "od_hal_reset.h"
#include "od_hal_secret.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_sec2_wire.h"
#include "od_txq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_checks, g_failures;
static const char *g_case = "";
#define CHECK(cond)                                                                          \
    do {                                                                                     \
        ++g_checks;                                                                          \
        if (!(cond)) {                                                                       \
            ++g_failures;                                                                    \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);               \
        }                                                                                    \
    } while (0)
#define CASE(name) (g_case = (name))

/* ---------------------------------------------------------------- persistent storage --- */

struct secret { bool present; uint16_t len; uint8_t data[32]; };
struct storage {
    struct secret sec[3];
    bool          rec_present;
    uint32_t      rec_len;
    uint8_t       rec[OD_CONFIG_STORE_MAX_RECORD];
};
static struct storage g_st;

/* Power-cut injection: the mutation numbered g_cut_at (0-based) does not happen, and nothing
 * after it does either (the power is gone); g_cut_snap is the storage the next boot finds. */
static int g_mut;
static int g_cut_at = -1;
static bool g_dead;
static struct storage g_cut_snap;
static int g_erase_fail_key;       /* OS2K erases that fail */
static unsigned g_os2s_writes;

static bool mutate(void)
{
    if (g_dead) {
        return false;
    }
    if (g_mut++ == g_cut_at) {
        g_cut_snap = g_st;
        g_dead = true;
        return false;
    }
    return true;
}

bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *buf, uint16_t len)
{
    if (!g_st.sec[id].present || g_st.sec[id].len != len) { return false; }
    memcpy(buf, g_st.sec[id].data, len);
    return true;
}
bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len)
{
    if (!mutate()) { return false; }
    memcpy(g_st.sec[id].data, buf, len);
    g_st.sec[id].len = len;
    g_st.sec[id].present = true;
    if (id == OD_HAL_SECRET_STATE) { ++g_os2s_writes; }
    return true;
}
bool od_hal_secret_erase(enum od_hal_secret_id id)
{
    if (id == OD_HAL_SECRET_KEY && g_erase_fail_key > 0) {
        --g_erase_fail_key;
        return false;
    }
    if (!mutate()) { return false; }
    memset(&g_st.sec[id], 0, sizeof g_st.sec[id]);
    return true;
}

int od_hal_nvs_init(void) { return OD_HAL_NVS_OK; }
int od_hal_nvs_size(uint32_t *len)
{
    if (!g_st.rec_present) { return OD_HAL_NVS_ENOENT; }
    *len = g_st.rec_len;
    return OD_HAL_NVS_OK;
}
int od_hal_nvs_read(uint32_t off, void *buf, uint32_t len)
{
    if (!g_st.rec_present) { return OD_HAL_NVS_ENOENT; }
    if (off + len > g_st.rec_len) { return OD_HAL_NVS_E2BIG; }
    memcpy(buf, g_st.rec + off, len);
    return OD_HAL_NVS_OK;
}
int od_hal_nvs_write(const void *rec, uint32_t len)
{
    if (!mutate()) { return OD_HAL_NVS_EIO; }
    memcpy(g_st.rec, rec, len);
    g_st.rec_len = len;
    g_st.rec_present = true;
    return OD_HAL_NVS_OK;
}
int od_hal_nvs_erase(void)
{
    if (!mutate()) { return OD_HAL_NVS_EIO; }
    g_st.rec_present = false;
    g_st.rec_len = 0u;
    return OD_HAL_NVS_OK;
}

/* ------------------------------------------------------------ crypto (inert, keyed) --- */

enum od_hal_crypto_status od_hal_crypto_key_set(od_hal_crypto_slot_t s, const uint8_t k[16])
{ (void)s; (void)k; return OD_HAL_CRYPTO_OK; }
void od_hal_crypto_key_clear(od_hal_crypto_slot_t s) { (void)s; }
enum od_hal_crypto_status od_hal_crypto_ccm_encrypt(od_hal_crypto_slot_t s, const uint8_t *n,
        uint8_t nl, const uint8_t *a, uint8_t al, const uint8_t *p, uint16_t pl, uint8_t *c,
        uint16_t cc, uint16_t *cl)
{ (void)s; (void)n; (void)nl; (void)a; (void)al; (void)p; (void)pl; (void)c; (void)cc; (void)cl;
  return OD_HAL_CRYPTO_ERROR; }
enum od_hal_crypto_status od_hal_crypto_ccm_decrypt(od_hal_crypto_slot_t s, const uint8_t *n,
        uint8_t nl, const uint8_t *a, uint8_t al, const uint8_t *c, uint16_t cl, uint8_t *p,
        uint16_t pc, uint16_t *pl)
{ (void)s; (void)n; (void)nl; (void)a; (void)al; (void)c; (void)cl; (void)p; (void)pc; (void)pl;
  return OD_HAL_CRYPTO_ERROR; }
/* Not CMAC: a keyed mix, enough for "equal input, equal digest" and the KDF's plumbing. */
enum od_hal_crypto_status od_hal_crypto_cmac(const uint8_t key[16], const uint8_t *m, uint32_t n,
                                             uint8_t out[16])
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < 16u; ++i) { h = (h ^ key[i]) * 16777619u; }
    for (uint32_t i = 0; i < n; ++i) { h = (h ^ m[i]) * 16777619u; }
    for (uint32_t i = 0; i < 16u; ++i) { h = (h ^ i) * 16777619u; out[i] = (uint8_t)(h >> 24); }
    return OD_HAL_CRYPTO_OK;
}
enum od_hal_crypto_status od_hal_crypto_aes_ecb(const uint8_t k[16], const uint8_t in[16],
                                                uint8_t out[16])
{ return od_hal_crypto_cmac(k, in, 16u, out); }
enum od_hal_crypto_status od_hal_crypto_random(uint8_t *b, uint16_t n)
{ for (uint16_t i = 0; i < n; ++i) { b[i] = (uint8_t)(rand() & 0xFF); } return OD_HAL_CRYPTO_OK; }
enum od_hal_crypto_status od_hal_crypto_ecdh_keygen(uint8_t pub[64])
{ (void)pub; return OD_HAL_CRYPTO_ERROR; }
enum od_hal_crypto_status od_hal_crypto_ecdh_agree(const uint8_t peer[64], uint8_t z[32])
{ (void)peer; (void)z; return OD_HAL_CRYPTO_ERROR; }
void od_hal_crypto_ecdh_discard(void) { }

od_radio_result_t od_hal_radio_send(od_origin_t o, uint32_t t, const uint8_t *f, uint16_t n)
{ (void)o; (void)t; (void)f; (void)n; return OD_RADIO_SENT; }
bool od_hal_radio_tag_is_live(od_origin_t o, uint32_t t) { (void)o; (void)t; return true; }
void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{ (void)rp; (void)len; (void)why; }

/* ------------------------------------------------------------------ the target hooks --- */

static uint32_t g_now;               /* uptime, ms */
static uint32_t g_cause;
static uint8_t  g_retained[16];
static unsigned g_reboots;
static unsigned g_draws[4];
static uint32_t g_last_code;
static unsigned g_signals[16];
static uint8_t  g_last_count_arg;
static struct SecurityConfig g_parsed;
static bool g_parsed_present;

/* reset pin: a scripted trace, one value per 10 ms sample */
static const uint8_t *g_pin_trace;
static uint32_t g_pin_n, g_pin_i;

uint32_t od_sec2_app_now_ms(void) { return g_now; }
void od_sec2_app_uid(uint8_t uid[8]) { memset(uid, 0x42, 8u); }
uint8_t od_sec2_app_dflags(void) { return 0u; }
const struct SecurityConfig *od_sec2_app_security(void) { return g_parsed_present ? &g_parsed : NULL; }
bool od_sec2_app_display_busy(void) { return false; }

uint32_t od_hal_reset_cause(void) { return g_cause; }
bool od_hal_retained_read(uint8_t b[16]) { memcpy(b, g_retained, 16u); return true; }
bool od_hal_retained_write(const uint8_t b[16]) { memcpy(g_retained, b, 16u); return true; }
void od_hal_retained_clear(void) { memset(g_retained, 0, 16u); }
void od_hal_reset_reboot(void) { ++g_reboots; }

bool od_claim_app_draw(enum od_setup_screen kind, uint32_t code)
{
    ++g_draws[kind];
    g_last_code = code;
    return true;
}
void od_claim_app_signal(enum od_claim_signal sig, uint8_t arg)
{
    ++g_signals[sig];
    if (sig == OD_CLAIM_SIG_PATTERN_COUNT) { g_last_count_arg = arg; }
}
bool od_claim_app_pin_input(uint8_t pin, bool up, bool down) { (void)pin; (void)up; (void)down; return true; }
int od_claim_app_pin_read(uint8_t pin)
{
    (void)pin;
    if (g_pin_trace == NULL) { return 1; }
    return g_pin_i < g_pin_n ? g_pin_trace[g_pin_i++] : g_pin_trace[g_pin_n - 1u];
}
void od_claim_app_wait_ms(uint32_t ms) { g_now += ms; }
bool od_claim_app_erase_images(void) { return mutate() || g_dead; }
bool od_claim_app_erase_bonds(void) { return true; }
bool od_claim_app_scrub_extras(void) { return true; }
static unsigned g_purges;
static bool g_purge_fails;
bool od_claim_app_storage_purge(void) { ++g_purges; return !g_purge_fails; }
static uint8_t s_ws[OD_CONFIG_STORE_MAX_RECORD];
uint8_t *od_claim_app_workspace(uint32_t *cap) { *cap = sizeof s_ws; return s_ws; }
bool od_claim_app_workspace_busy(void) { return false; }

static bool find27(void *ctx, uint8_t id, od_span_t body)
{
    (void)ctx;
    if (id == 0x27u && body.n >= sizeof g_parsed) {
        memcpy(&g_parsed, body.p, sizeof g_parsed);
        g_parsed_present = true;
    }
    return true;
}
void od_claim_app_config_changed(void)
{
    uint8_t b[OD_CONFIG_MAX_SIZE];
    uint32_t n = sizeof b;

    g_parsed_present = false;
    if (od_config_store_load(b, &n) == OD_CONFIG_STORE_OK) {
        (void)od_config_tlv_walk(od_span_make(b, n), find27, NULL, NULL, NULL);
    }
}

/* ------------------------------------------------------------------------ the device --- */

static struct od_security_boot_report g_rep;

/* One boot with `cause`; the retained RAM survives only a software reset. */
static void boot(uint32_t cause)
{
    g_now = 5u;
    g_cause = cause;
    if (cause != OD_HAL_RESET_SOFTWARE) {
        memset(g_retained, 0xA5, sizeof g_retained);   /* garbage after power loss */
    }
    memset(g_draws, 0, sizeof g_draws);
    memset(g_signals, 0, sizeof g_signals);
    g_reboots = 0u;
    od_claim_mode_test_reset();
    od_claim_app_config_changed();
    od_security_boot(g_parsed_present ? &g_parsed : NULL, g_parsed_present, false, &g_rep);
}

/* Run the loop until `until` ms of uptime, ticking every 50 ms. Stops at a reboot. */
static void run_until(uint32_t until)
{
    while (g_now < until && g_reboots == 0u) {
        g_now += 50u;
        od_claim_mode_tick();
    }
}

static uint8_t short_boots(void)
{
    struct od_sec2_state_rec s;
    return (g_st.sec[1].present && od_sec2_os2s_decode(g_st.sec[1].data, &s)) ? s.short_boots : 0u;
}

static uint8_t os2s_flags(void)
{
    struct od_sec2_state_rec s;
    return (g_st.sec[1].present && od_sec2_os2s_decode(g_st.sec[1].data, &s)) ? s.flags : 0xFFu;
}

static void wipe_device(void)
{
    memset(&g_st, 0, sizeof g_st);
    memset(g_retained, 0, sizeof g_retained);
    g_cut_at = -1;
    g_dead = false;
    g_mut = 0;
    g_erase_fail_key = 0;
    g_pin_trace = NULL;
}

static const uint8_t K1[16] = { 9,8,7,6,5,4,3,2,1,0,1,2,3,4,5,6 };

/* A configured device: config with a WiFi password (optional), 0x27 at rest when claimed. */
static void provision(bool claimed, bool secret, uint8_t pin_flags, uint8_t pin)
{
    uint8_t b[400];
    uint32_t n = 3u;
    uint16_t c;
    static uint8_t ws[OD_CONFIG_STORE_MAX_RECORD];

    memset(b, 0, sizeof b);
    b[2] = 1u;
    b[n++] = 0u; b[n++] = 0x26u; b[n] = 'N'; b[n + 32u] = secret ? 0x77u : 0u; n += 160u;
    b[n++] = 0u; b[n++] = 0x27u;
    if (claimed) {
        b[n] = 1u;
        memcpy(b + n + 1u, K1, 16u);
        b[n + 21u] = OD_SEC27_AT_REST_SEC2;
    }
    b[n + 19u] = pin_flags;
    b[n + 20u] = pin;
    n += 64u;
    c = od_config_tlv_crc16(od_span_make(b, n));
    b[n++] = (uint8_t)c;
    b[n++] = (uint8_t)(c >> 8);
    CHECK(od_config_store_save(ws, sizeof ws, b, n) == OD_CONFIG_STORE_OK);
    if (claimed) {
        struct od_sec2_key_rec k = { OD_SEC2_ORIGIN_LEGACY, 1u, {0} };
        struct od_sec2_state_rec s = { OD_SEC2_OS2S_MIGRATED, 0u, 1u };
        uint8_t r[OD_SEC2_OS2K_LEN], t[OD_SEC2_OS2S_LEN];
        memcpy(k.key, K1, 16u);
        od_sec2_os2k_encode(&k, r);
        od_sec2_os2s_encode(&s, t);
        CHECK(od_hal_secret_write(OD_HAL_SECRET_KEY, r, sizeof r));
        CHECK(od_hal_secret_write(OD_HAL_SECRET_STATE, t, sizeof t));
    } else {
        struct od_sec2_state_rec s = { OD_SEC2_OS2S_MIGRATED, 0u, 0u };
        uint8_t t[OD_SEC2_OS2S_LEN];
        od_sec2_os2s_encode(&s, t);
        CHECK(od_hal_secret_write(OD_HAL_SECRET_STATE, t, sizeof t));
    }
}

/* One quick power cycle: power on, live `on_ms`, power cut. */
static void cycle(uint32_t on_ms)
{
    boot(OD_HAL_RESET_POWER_ON);
    if (g_reboots == 0u) {
        run_until(on_ms);
    }
}

/* ---------------------------------------------------------------------------- tests --- */

static void test_causes(void)
{
    CASE("counted causes: power-on, the reset pin, nRF's power-on-or-brown-out; nothing else");
    CHECK(od_claim_cause_counted(OD_HAL_RESET_POWER_ON));
    CHECK(od_claim_cause_counted(OD_HAL_RESET_PIN));
    CHECK(od_claim_cause_counted(OD_HAL_RESET_POWER_ON_OR_BROWNOUT));
    CHECK(od_claim_cause_counted(OD_HAL_RESET_POWER_ON | OD_HAL_RESET_PIN));
    CHECK(!od_claim_cause_counted(0u));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_BROWNOUT));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_SOFTWARE));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_WATCHDOG));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_LOCKUP));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_SLEEP_WAKE));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_DEBUG));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_OTHER));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_PIN | OD_HAL_RESET_SLEEP_WAKE));
    CHECK(!od_claim_cause_counted(OD_HAL_RESET_POWER_ON | OD_HAL_RESET_WATCHDOG));
}

static void test_history_edges(void)
{
    static const uint32_t uncounted[] = {
        OD_HAL_RESET_BROWNOUT, OD_HAL_RESET_SOFTWARE, OD_HAL_RESET_WATCHDOG, OD_HAL_RESET_LOCKUP,
        OD_HAL_RESET_SLEEP_WAKE, OD_HAL_RESET_DEBUG, OD_HAL_RESET_OTHER };

    CASE("a counted boot arms at 2.0 s, not at 1.95 s, and clears at 10.0 s");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.kind == OD_CLAIM_KIND_NONE);
    run_until(1950u);
    CHECK(short_boots() == 0u);
    run_until(2000u);
    CHECK(short_boots() == 1u);
    CHECK(g_signals[OD_CLAIM_SIG_PATTERN_COUNT] == 1u && g_last_count_arg == 1u);
    run_until(9950u);
    CHECK(short_boots() == 1u);
    run_until(10000u);
    CHECK(short_boots() == 0u);

    CASE("a loop blocked past T_ARM (a panel refresh) still arms at 2 s via tick_count");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    boot(OD_HAL_RESET_POWER_ON);
    while (g_now < 5600u) {          /* one 5.6 s refresh: only the busy-wait hook runs */
        g_now += 5u;
        od_claim_mode_tick_count();
        if (g_now == 2000u) {
            CHECK(short_boots() == 1u);
        }
    }
    CHECK(g_draws[OD_SETUP_SCREEN_SETUP] == 0u && g_reboots == 0u);
    run_until(10000u);
    CHECK(short_boots() == 0u);

    CASE("wear: two OS2S writes for a boot that lives past 10 s, one if cut in 2-10 s, none < 2 s");
    g_os2s_writes = 0u;
    cycle(20000u);
    CHECK(g_os2s_writes == 2u);
    g_os2s_writes = 0u;
    cycle(5000u);
    CHECK(g_os2s_writes == 1u);
    boot(OD_HAL_RESET_SOFTWARE);    /* clear it again */
    g_os2s_writes = 0u;
    cycle(1900u);
    CHECK(g_os2s_writes == 0u);

    CASE("cut at 1.9 s does not count; cut at 9.9 s counts (unclaimed: GESTURE next)");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    cycle(1900u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.short_boots == 0u && g_rep.kind == OD_CLAIM_KIND_NONE);
    run_until(9900u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.short_boots == 1u && g_rep.kind == OD_CLAIM_KIND_GESTURE);

    for (size_t i = 0; i < sizeof uncounted / sizeof uncounted[0]; ++i) {
        CASE("every uncounted reset clears a pending count and opens nothing");
        wipe_device();
        provision(false, false, 0u, 0xFFu);
        cycle(5000u);
        CHECK(short_boots() == 1u);
        boot(uncounted[i]);
        CHECK(g_rep.kind == OD_CLAIM_KIND_NONE);
        CHECK(short_boots() == 0u);
        run_until(5000u);
        CHECK(short_boots() == 0u);              /* an uncounted boot never arms */
    }

    CASE("a reset-pin reset counts like a power-on");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    boot(OD_HAL_RESET_PIN);
    run_until(3000u);
    boot(OD_HAL_RESET_PIN);
    CHECK(g_rep.kind == OD_CLAIM_KIND_GESTURE);

    CASE("deep sleep / planned reboot inside the window disarm the count");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    boot(OD_HAL_RESET_POWER_ON);
    run_until(3000u);
    CHECK(short_boots() == 1u);
    CHECK(od_claim_mode_before_sleep());
    CHECK(short_boots() == 0u);
    boot(OD_HAL_RESET_POWER_ON);
    run_until(3000u);
    CHECK(od_claim_mode_before_reboot());
    CHECK(short_boots() == 0u);
}

static void test_gesture_claimed(void)
{
    CASE("claimed: the gesture opens nothing and draws nothing, but the count continues");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    cycle(3000u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.state == OD_SEC2_CLAIMED && g_rep.kind == OD_CLAIM_KIND_NONE);
    CHECK(g_rep.pattern == OD_PATTERN_CLAIM);
    CHECK(!od_claim_mode_active());
    CHECK(od_claim_mode_defer_boot_screen());
    run_until(3000u);
    CHECK(short_boots() == 2u);
    CHECK(g_draws[OD_SETUP_SCREEN_SETUP] == 0u);
    run_until(10000u);
    CHECK(!od_claim_mode_defer_boot_screen());
}

static void test_five_cycles(void)
{
    unsigned i;

    CASE("five quick power-cycles factory-reset a CLAIMED device; it comes back FRESH");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (i = 0; i < 4u; ++i) {
        cycle(3000u);
        CHECK(g_rep.pattern != OD_PATTERN_FACTORY_RESET);
    }
    CHECK(short_boots() == 4u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.pattern == OD_PATTERN_FACTORY_RESET);
    CHECK(g_reboots == 1u);
    CHECK(g_draws[OD_SETUP_SCREEN_RESET] == 1u);
    CHECK(!g_st.sec[OD_HAL_SECRET_KEY].present && !g_st.sec[OD_HAL_SECRET_MARKER].present);
    CHECK(!g_st.rec_present);
    CHECK((os2s_flags() & OD_SEC2_OS2S_FRESH) != 0u);
    CHECK((os2s_flags() & OD_SEC2_OS2S_PURGE) != 0u);   /* R6 asks the next boot to purge */
    CHECK(short_boots() == 0u);
    g_purges = 0u;
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.state == OD_SEC2_UNCLAIMED && g_rep.kind == OD_CLAIM_KIND_FRESH);
    CHECK(g_purges == 1u && g_rep.purged && (os2s_flags() & OD_SEC2_OS2S_PURGE) == 0u);
    CHECK((os2s_flags() & OD_SEC2_OS2S_FRESH) != 0u);  /* the purge keeps FRESH */
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_purges == 1u && !g_rep.purged);            /* once */

    CASE("a failed purge keeps the flag; the next boot retries");
    {
        struct od_sec2_state_rec st = { (uint8_t)(OD_SEC2_OS2S_MIGRATED | OD_SEC2_OS2S_PURGE), 0u, 2u };
        uint8_t t[OD_SEC2_OS2S_LEN];

        wipe_device();
        provision(false, false, 0u, 0xFFu);
        od_sec2_os2s_encode(&st, t);
        CHECK(od_hal_secret_write(OD_HAL_SECRET_STATE, t, sizeof t));
        g_purges = 0u;
        g_purge_fails = true;
        boot(OD_HAL_RESET_SOFTWARE);
        CHECK(g_purges == 1u && !g_rep.purged && (os2s_flags() & OD_SEC2_OS2S_PURGE) != 0u);
        CHECK(g_rep.state == OD_SEC2_UNCLAIMED && g_rep.kind == OD_CLAIM_KIND_NONE);
        g_purge_fails = false;
        boot(OD_HAL_RESET_SOFTWARE);
        CHECK(g_purges == 2u && g_rep.purged && (os2s_flags() & OD_SEC2_OS2S_PURGE) == 0u);
        CHECK((os2s_flags() & OD_SEC2_OS2S_MIGRATED) != 0u);
    }

    CASE("three quick cycles, then a long boot: no reset, the count is gone");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (i = 0; i < 3u; ++i) { cycle(3000u); }
    cycle(20000u);
    CHECK(g_rep.pattern != OD_PATTERN_FACTORY_RESET && short_boots() == 0u);
    cycle(3000u);
    CHECK(g_rep.pattern == OD_PATTERN_NONE && g_rep.state == OD_SEC2_CLAIMED);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.pattern == OD_PATTERN_CLAIM && g_st.sec[OD_HAL_SECRET_KEY].present);

    CASE("a brown-out or watchdog in the middle restarts the pattern");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (i = 0; i < 3u; ++i) { cycle(3000u); }
    boot(OD_HAL_RESET_BROWNOUT);
    for (i = 0; i < 2u; ++i) { cycle(3000u); }
    CHECK(g_rep.pattern != OD_PATTERN_FACTORY_RESET);
    CHECK(g_st.sec[OD_HAL_SECRET_KEY].present);

    CASE("unclaimed: claim mode does not swallow the count; SETUP waits out the pattern");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    cycle(3000u);
    cycle(3000u);                                 /* k=1: GESTURE, SETUP drawn (k < 2) */
    CHECK(g_rep.kind == OD_CLAIM_KIND_GESTURE && g_draws[OD_SETUP_SCREEN_SETUP] == 1u);
    cycle(3000u);                                 /* k=2: GESTURE, no SETUP before 10 s */
    CHECK(g_rep.kind == OD_CLAIM_KIND_GESTURE && g_draws[OD_SETUP_SCREEN_SETUP] == 0u);
    cycle(3000u);
    CHECK(g_draws[OD_SETUP_SCREEN_SETUP] == 0u);
    boot(OD_HAL_RESET_POWER_ON);                  /* k=4 */
    CHECK(g_rep.pattern == OD_PATTERN_FACTORY_RESET && g_reboots == 1u);

    CASE("k=2 in claim mode, then left on: SETUP is drawn at 10 s");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    cycle(3000u);
    cycle(3000u);
    boot(OD_HAL_RESET_POWER_ON);
    run_until(9950u);
    CHECK(g_draws[OD_SETUP_SCREEN_SETUP] == 0u);
    run_until(10100u);
    CHECK(g_draws[OD_SETUP_SCREEN_SETUP] == 1u);

    CASE("LOCKED (claim evidence, no key): five cycles reset it too");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    memset(&g_st.sec[OD_HAL_SECRET_KEY], 0, sizeof g_st.sec[0]);
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.state == OD_SEC2_LOCKED);
    for (i = 0; i < 4u; ++i) { cycle(3000u); }
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.pattern == OD_PATTERN_FACTORY_RESET && g_reboots == 1u);
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.state == OD_SEC2_UNCLAIMED && g_rep.kind == OD_CLAIM_KIND_FRESH);
}

static void test_session(void)
{
    struct od_sec2_claim_mode cm;

    CASE("virgin (no OS2S, no config): FRESH, OS2S {FRESH, MIGRATED} written");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.kind == OD_CLAIM_KIND_FRESH);
    CHECK(os2s_flags() == (OD_SEC2_OS2S_FRESH | OD_SEC2_OS2S_MIGRATED));
    od_claim_mode_status(&cm);
    CHECK(cm.active && cm.fresh && !cm.config_locked && cm.renders_left == 3u && cm.window_s == 600u);
    run_until(100u);
    CHECK(g_draws[OD_SETUP_SCREEN_SETUP] == 1u);

    CASE("a configured, never-claimed 2.x device is not virgin: normal mode");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    memset(&g_st.sec[OD_HAL_SECRET_STATE], 0, sizeof g_st.sec[0]);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.kind == OD_CLAIM_KIND_NONE);

    CASE("three code renders end the session (ATTEMPTS) and clear FRESH (Q15)");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    od_claim_mode_event(OD_SEC2_EV_CODE, 123456u);
    run_until(1000u);
    CHECK(g_draws[OD_SETUP_SCREEN_CODE] == 1u && g_last_code == 123456u);
    od_claim_mode_status(&cm);
    CHECK(cm.config_locked && cm.renders_left == 2u);
    od_claim_mode_event(OD_SEC2_EV_DISCARDED, 1u);
    od_claim_mode_event(OD_SEC2_EV_CODE, 1u);
    od_claim_mode_event(OD_SEC2_EV_DISCARDED, 1u);
    od_claim_mode_event(OD_SEC2_EV_CODE, 2u);
    CHECK(od_claim_mode_end_reason() == OD_CLAIM_END_NONE);
    od_claim_mode_event(OD_SEC2_EV_DISCARDED, 1u);
    CHECK(od_claim_mode_end_reason() == OD_CLAIM_END_ATTEMPTS);
    CHECK(od_claim_mode_active());                /* until the reboot: ATTEMPTS_EXHAUSTED answers */
    CHECK((os2s_flags() & OD_SEC2_OS2S_FRESH) == 0u);
    run_until(g_now + 600u);
    CHECK(g_reboots == 1u);
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.kind == OD_CLAIM_KIND_NONE && !g_rep.continued);

    CASE("the deadline: TIMEOUT after 600 s; a pending code extends it to its CONFIRM");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    run_until(599000u);
    od_claim_mode_event(OD_SEC2_EV_CODE, 7u);
    run_until(700000u);
    CHECK(g_reboots == 0u);
    run_until(905000u);
    CHECK(od_claim_mode_end_reason() == OD_CLAIM_END_TIMEOUT && g_reboots == 1u);

    CASE("EXIT before a claim: CANCEL; claimed then disconnect: CLAIMED");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    od_claim_mode_event(OD_SEC2_EV_EXIT, 0u);
    CHECK(od_claim_mode_end_reason() == OD_CLAIM_END_CANCEL);
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    od_claim_mode_event(OD_SEC2_EV_CODE, 7u);
    od_claim_mode_event(OD_SEC2_EV_CLAIMED, 0u);
    CHECK(g_signals[OD_CLAIM_SIG_CM_CLAIMED] == 1u);
    run_until(2000u);
    CHECK(g_draws[OD_SETUP_SCREEN_CODE] == 0u);   /* the claim landed first: no code redraw */
    CHECK(od_claim_mode_active() && od_claim_mode_end_reason() == OD_CLAIM_END_NONE);
    od_claim_mode_event(OD_SEC2_EV_DISCONNECTED, 0u);
    CHECK(od_claim_mode_end_reason() == OD_CLAIM_END_CLAIMED);

    CASE("claim mode never deep-sleeps");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(!od_claim_mode_before_sleep());

    CASE("a planned reboot CONTINUEs: renders and restarts carry, the 4th restart ends it");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    od_claim_mode_event(OD_SEC2_EV_CODE, 7u);
    od_claim_mode_event(OD_SEC2_EV_DISCARDED, 1u);
    CHECK(od_claim_mode_before_reboot());
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.continued && g_rep.kind == OD_CLAIM_KIND_FRESH);
    CHECK(od_claim_mode_renders_used() == 1u && od_claim_mode_restarts_used() == 1u);
    od_claim_mode_status(&cm);
    CHECK(cm.config_locked && cm.renders_left == 2u);
    (void)od_claim_mode_before_reboot();
    boot(OD_HAL_RESET_SOFTWARE);
    (void)od_claim_mode_before_reboot();
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.continued && od_claim_mode_restarts_used() == 3u);
    (void)od_claim_mode_before_reboot();
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(od_claim_mode_end_reason() == OD_CLAIM_END_RESTART_CAP);
    run_until(1000u);
    CHECK(g_draws[OD_SETUP_SCREEN_SETUP] == 0u && g_reboots == 1u);

    CASE("OS2C after a power loss is garbage: no CONTINUE (FRESH starts a new session)");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    od_claim_mode_event(OD_SEC2_EV_CODE, 7u);
    (void)od_claim_mode_before_reboot();
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(!g_rep.continued && g_rep.kind == OD_CLAIM_KIND_FRESH && od_claim_mode_renders_used() == 0u);

    CASE("a valid OS2C with a non-software cause is not a CONTINUE");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    cycle(3000u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.kind == OD_CLAIM_KIND_GESTURE);
    (void)od_claim_mode_before_reboot();
    g_cause = OD_HAL_RESET_WATCHDOG;
    od_claim_mode_test_reset();
    od_security_boot(NULL, true, false, &g_rep);
    CHECK(!g_rep.continued && g_rep.kind == OD_CLAIM_KIND_NONE);

    CASE("claimed inside claim mode, then a planned reboot: END(CLAIMED), normal boot");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    od_claim_mode_event(OD_SEC2_EV_CLAIMED, 0u);
    CHECK(od_claim_mode_before_reboot());
    CHECK(od_claim_mode_end_reason() == OD_CLAIM_END_CLAIMED);
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(!g_rep.continued && g_rep.kind == OD_CLAIM_KIND_NONE);
}

/* ------------------------------------------------------------------ factory reset --- */

static enum od_fr_pin_verdict pin_run(const uint8_t *trace, uint32_t n)
{
    struct od_fr_pin p;
    enum od_fr_pin_verdict v = OD_FR_PIN_RUNNING;
    uint32_t t = 0u;

    od_fr_pin_init(&p);
    for (uint32_t i = 0; v == OD_FR_PIN_RUNNING; ++i, t += OD_FR_SAMPLE_MS) {
        v = od_fr_pin_step(&p, (i < n ? trace[i] : trace[n - 1u]) != 0u, t);
    }
    return v;
}

static uint8_t g_trace[4000];

static uint32_t held(uint32_t ms)
{
    const uint32_t n = ms / OD_FR_SAMPLE_MS;
    memset(g_trace, 1, n);
    memset(g_trace + n, 0, 20u);
    return n + 20u;
}

static void test_pin(void)
{
    CASE("pin: not asserted at the first read is NORMAL at once");
    g_trace[0] = 0u;
    CHECK(pin_run(g_trace, 1u) == OD_FR_PIN_NORMAL);
    CASE("pin: released at 4.99 s is NORMAL, at 5.00 s COMMIT, at 29.99 s COMMIT");
    CHECK(pin_run(g_trace, held(4990u)) == OD_FR_PIN_NORMAL);
    CHECK(pin_run(g_trace, held(5000u)) == OD_FR_PIN_COMMIT);
    CHECK(pin_run(g_trace, held(29990u)) == OD_FR_PIN_COMMIT);
    CASE("pin: still held at 30 s is STUCK");
    memset(g_trace, 1, 3100u);
    CHECK(pin_run(g_trace, 3100u) == OD_FR_PIN_STUCK);
    CASE("pin: a 40 ms glitch at boot is NORMAL; a 40 ms bounce during the hold is ignored");
    memset(g_trace, 0, 50u);
    memset(g_trace, 1, 4u);
    CHECK(pin_run(g_trace, 50u) == OD_FR_PIN_NORMAL);
    held(6000u);
    memset(g_trace + 200u, 0, 4u);
    CHECK(pin_run(g_trace, 620u) == OD_FR_PIN_COMMIT);
    CASE("pin: a release 50 ms long inside the first 5 s ends the hold (NORMAL)");
    held(6000u);
    memset(g_trace + 200u, 0, 5u);
    CHECK(pin_run(g_trace, 620u) == OD_FR_PIN_NORMAL);

    CASE("the reset pin, held 6 s on a CLAIMED device: factory reset, RESET drawn once");
    wipe_device();
    provision(true, true, OD_SECURITY_FLAG_RESET_PIN_ENABLED, 3u);   /* low = asserted */
    {
        static uint8_t lv[700];
        memset(lv, 0, 600u);
        memset(lv + 600u, 1, 100u);
        g_pin_trace = lv; g_pin_n = 700u; g_pin_i = 0u;
        boot(OD_HAL_RESET_SLEEP_WAKE);          /* a button wake: the user keeps holding */
        g_pin_trace = NULL;
    }
    CHECK(g_reboots == 1u && g_draws[OD_SETUP_SCREEN_RESET] == 1u);
    CHECK(g_signals[OD_CLAIM_SIG_FR_HOLDING] == 1u && g_signals[OD_CLAIM_SIG_FR_RELEASE_NOW] == 1u);
    CHECK(!g_st.sec[OD_HAL_SECRET_KEY].present && !g_st.rec_present);

    CASE("pin disabled in 0x27: never sampled");
    wipe_device();
    provision(true, true, 0u, 3u);
    {
        static const uint8_t low[1] = { 0u };
        g_pin_trace = low; g_pin_n = 1u; g_pin_i = 0u;
        boot(OD_HAL_RESET_POWER_ON);
        CHECK(g_pin_i == 0u && g_reboots == 0u);
        g_pin_trace = NULL;
    }
}

static bool stored_secret_free(void)
{
    return !od_cfg_policy_stored_has_secrets();
}

/* Run boots (software resets) until one returns without a reboot; at most 8. */
static void settle(uint32_t first_cause)
{
    boot(first_cause);
    for (int i = 0; i < 8 && g_reboots != 0u; ++i) {
        boot(OD_HAL_RESET_SOFTWARE);
    }
}

/* 0x27 flags bit 6, NO_RESET_PATTERN (SECURITY_V2 7.10, FACTORY_RESET.md 3.4). */
static void test_reset_pattern_flag(void)
{
    const uint8_t off = (uint8_t)OD_SECURITY_FLAG_NO_RESET_PATTERN;
    unsigned i;

    CASE("pattern off, CLAIMED: five and more quick cycles reset nothing and count nothing");
    wipe_device();
    provision(true, true, off, 0xFFu);
    g_os2s_writes = 0u;
    for (i = 0; i < 6u; ++i) {
        cycle(3000u);
        CHECK(g_rep.pattern == OD_PATTERN_NONE && g_rep.state == OD_SEC2_CLAIMED && g_reboots == 0u);
    }
    CHECK(short_boots() == 0u && g_os2s_writes == 0u);   /* no history: no wear either */
    CHECK(g_st.sec[OD_HAL_SECRET_KEY].present && g_st.rec_present);

    CASE("pattern off, CLAIMED: the reset pin still resets");
    wipe_device();
    provision(true, true, (uint8_t)(off | OD_SECURITY_FLAG_RESET_PIN_ENABLED), 3u);
    {
        static uint8_t lv[700];
        memset(lv, 0, 600u);
        memset(lv + 600u, 1, 100u);
        g_pin_trace = lv; g_pin_n = 700u; g_pin_i = 0u;
        boot(OD_HAL_RESET_POWER_ON);
        g_pin_trace = NULL;
    }
    CHECK(g_reboots == 1u && !g_st.sec[OD_HAL_SECRET_KEY].present && !g_st.rec_present);

    CASE("pattern off, UNCLAIMED: the claim gesture still works; k >= 4 stays claim mode");
    wipe_device();
    provision(false, false, off, 0xFFu);
    cycle(3000u);
    cycle(3000u);
    CHECK(g_rep.kind == OD_CLAIM_KIND_GESTURE);
    for (i = 0; i < 3u; ++i) { cycle(3000u); }
    CHECK(short_boots() >= 4u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.pattern == OD_PATTERN_CLAIM && g_reboots == 0u && g_rep.kind == OD_CLAIM_KIND_GESTURE);
    CHECK(g_st.rec_present);

    CASE("pattern off, LOCKED: the stored flag is ignored, five cycles reset it");
    wipe_device();
    provision(true, true, off, 0xFFu);
    memset(&g_st.sec[OD_HAL_SECRET_KEY], 0, sizeof g_st.sec[0]);
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.state == OD_SEC2_LOCKED);
    for (i = 0; i < 4u; ++i) {
        cycle(3000u);
        CHECK(g_reboots == 0u);
    }
    CHECK(short_boots() == 4u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.pattern == OD_PATTERN_FACTORY_RESET && g_reboots == 1u);
    CHECK(!g_st.rec_present && !g_st.sec[OD_HAL_SECRET_MARKER].present);
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.state == OD_SEC2_UNCLAIMED && g_rep.kind == OD_CLAIM_KIND_FRESH);

    CASE("pattern on again: a config without bit 6 restores the default");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (i = 0; i < 4u; ++i) { cycle(3000u); }
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.pattern == OD_PATTERN_FACTORY_RESET && g_reboots == 1u);
}

static void test_power_cut_everywhere(void)
{
    int cut;
    int total;

    CASE("factory reset by the pattern: how many mutations a clean run takes");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (int i = 0; i < 4; ++i) { cycle(3000u); }
    g_mut = 0;
    boot(OD_HAL_RESET_POWER_ON);
    total = g_mut;
    CHECK(total >= 6);

    for (cut = 0; cut <= total; ++cut) {
        struct storage lost;

        CASE("A4: a power cut at every mutation; the marker lost there too");
        wipe_device();
        provision(true, true, 0u, 0xFFu);
        for (int i = 0; i < 4; ++i) { cycle(3000u); }
        g_mut = 0;
        g_cut_at = cut;
        g_dead = false;
        boot(OD_HAL_RESET_POWER_ON);
        if (!g_dead) {
            continue;
        }
        g_st = g_cut_snap;
        g_cut_at = -1;
        g_dead = false;

        /* (1) the marker lost at the cut: the boot decision must not see an open device with
         * a secret. */
        lost = g_st;
        memset(&g_st.sec[OD_HAL_SECRET_MARKER], 0, sizeof g_st.sec[0]);
        boot(OD_HAL_RESET_SOFTWARE);
        CHECK(g_rep.state != OD_SEC2_UNCLAIMED || stored_secret_free());

        /* (2) the marker kept: the reset finishes, the device comes back FRESH and secret-free. */
        g_st = lost;
        settle(OD_HAL_RESET_POWER_ON);
        CHECK(g_reboots == 0u);
        if (g_st.sec[OD_HAL_SECRET_KEY].present) {
            /* only before R1 landed: no reset began, the device is untouched and claimed */
            CHECK(g_rep.state == OD_SEC2_CLAIMED);
        } else {
            CHECK(g_rep.state == OD_SEC2_UNCLAIMED && g_rep.kind == OD_CLAIM_KIND_FRESH);
            CHECK(stored_secret_free());
            CHECK(!g_st.sec[OD_HAL_SECRET_MARKER].present);
        }
    }

    CASE("R2: the key erase fails twice, then succeeds: two reboots, then done");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (int i = 0; i < 4; ++i) { cycle(3000u); }
    g_erase_fail_key = 2;
    settle(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.state == OD_SEC2_UNCLAIMED && g_rep.kind == OD_CLAIM_KIND_FRESH);
    CHECK(!g_st.sec[OD_HAL_SECRET_KEY].present);

    CASE("R2: three key-erase failures: stage 4, LOCKED, no boot loop, secrets already gone");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (int i = 0; i < 4; ++i) { cycle(3000u); }
    g_erase_fail_key = 100;
    settle(OD_HAL_RESET_POWER_ON);
    CHECK(g_reboots == 0u && g_rep.reset_locked && g_rep.state == OD_SEC2_LOCKED);
    CHECK(stored_secret_free());
    CHECK(g_signals[OD_CLAIM_SIG_FR_LOCKED] == 1u);
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_rep.reset_locked && g_reboots == 0u);
    g_erase_fail_key = 0;

    CASE("R3: the draw counts its attempt first; after two the RESET screen is skipped");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    for (int i = 0; i < 4; ++i) { cycle(3000u); }
    {
        struct od_fr_marker m = { OD_FR_STAGE_DRAW, 2u, 0u };
        uint8_t r[OD_FR_OS2R_LEN];
        od_fr_marker_encode(&m, r);
        CHECK(od_hal_secret_write(OD_HAL_SECRET_MARKER, r, sizeof r));
    }
    boot(OD_HAL_RESET_SOFTWARE);
    CHECK(g_draws[OD_SETUP_SCREEN_RESET] == 0u && g_reboots == 1u);
}

static void test_marker_codec(void)
{
    struct od_fr_marker m = { 2u, 1u, 0u }, o;
    uint8_t r[OD_FR_OS2R_LEN];

    CASE("OS2R: round trip; any flipped bit, a bad stage or version is rejected");
    od_fr_marker_encode(&m, r);
    CHECK(memcmp(r, "OS2R\x02\x02\x01\x00", 8u) == 0);
    CHECK(od_fr_marker_decode(r, &o) && o.stage == 2u && o.draw_attempts == 1u);
    for (unsigned i = 0; i < sizeof r * 8u; ++i) {
        r[i / 8u] ^= (uint8_t)(1u << (i % 8u));
        CHECK(!od_fr_marker_decode(r, &o));
        r[i / 8u] ^= (uint8_t)(1u << (i % 8u));
    }
    m.stage = 5u;
    od_fr_marker_encode(&m, r);
    CHECK(!od_fr_marker_decode(r, &o));
    m.stage = 0u;
    od_fr_marker_encode(&m, r);
    CHECK(!od_fr_marker_decode(r, &o));
}


/* ------------------------------------------------------------- MSD in every state --- */

static const uint8_t DD[OD_ADVERT_DYNAMIC_LEN] = { 1,2,3,4,5,6,7,8,9,10,11 };

/* The 16 MSD bytes a target publishes now: its own inputs (dynamic DD, counter 0, no reboot),
 * then the shared device state, exactly as all three targets compose them. */
static void msd_now(uint8_t out[OD_ADVERT_MSD_LEN])
{
    struct od_advert_inputs in;
    struct od_advert_device_state st;

    memset(&in, 0, sizeof in);
    in.dynamic = DD;
    in.chip_temperature_c = 20.0f;
    in.battery_10mv = 300u;
    od_advert_device_state_now(&st);
    od_advert_apply_device_state(&in, &st);
    od_advert_build(&in, out);
}

static void check_msd(uint8_t bits23)
{
    uint8_t m[OD_ADVERT_MSD_LEN];

    msd_now(m);
    CHECK(m[0] == 0x46u && m[1] == 0x24u);
    CHECK(m[13] == 120u && m[14] == (uint8_t)300u && (m[15] & 0x01u) == 1u);
    CHECK((m[15] & 0x0Cu) == bits23);
    CHECK((m[15] & 0xF2u) == 0u);
}

/* A 2.x config with a v1 key in force and nothing of 3.0: boot row 4 migrates it. */
static void provision_v1_key(void)
{
    uint8_t b[200];
    uint32_t n = 3u;
    uint16_t c;
    static uint8_t ws[OD_CONFIG_STORE_MAX_RECORD];

    memset(b, 0, sizeof b);
    b[2] = 1u;
    b[n++] = 0u; b[n++] = 0x27u;
    b[n] = 1u;
    memcpy(b + n + 1u, K1, 16u);
    n += 64u;
    c = od_config_tlv_crc16(od_span_make(b, n));
    b[n++] = (uint8_t)c;
    b[n++] = (uint8_t)(c >> 8);
    CHECK(od_config_store_save(ws, sizeof ws, b, n) == OD_CONFIG_STORE_OK);
}

static void test_msd_states(void)
{
    CASE("MSD: virgin device = FRESH claim mode, bit2 set, bit3 clear");
    wipe_device();
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(od_claim_mode_active() && g_rep.kind == OD_CLAIM_KIND_FRESH);
    check_msd(OD_MSD_STATUS_CLAIM_MODE);

    CASE("MSD: unclaimed, normal mode: bits 2 and 3 clear");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(!od_claim_mode_active() && g_rep.state == OD_SEC2_UNCLAIMED);
    check_msd(0u);

    CASE("MSD: gesture claim mode: bit2 set, bit3 clear");
    wipe_device();
    provision(false, false, 0u, 0xFFu);
    cycle(3000u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(od_claim_mode_active() && g_rep.kind == OD_CLAIM_KIND_GESTURE);
    check_msd(OD_MSD_STATUS_CLAIM_MODE);

    CASE("MSD: claimed: bit3 set, bit2 clear (also after the gesture)");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.state == OD_SEC2_CLAIMED);
    check_msd(OD_MSD_STATUS_SESSION_REQUIRED);
    cycle(3000u);
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(!od_claim_mode_active());
    check_msd(OD_MSD_STATUS_SESSION_REQUIRED);

    CASE("MSD: LOCKED (claim marker, key record gone): bit3 set, bit2 clear");
    wipe_device();
    provision(true, true, 0u, 0xFFu);
    g_st.sec[0].present = false;
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.state == OD_SEC2_LOCKED);
    check_msd(OD_MSD_STATUS_SESSION_REQUIRED);

    CASE("MSD: a 2.x v1 key migrated at the first 3.0 boot: bit3 set");
    wipe_device();
    provision_v1_key();
    boot(OD_HAL_RESET_POWER_ON);
    CHECK(g_rep.state == OD_SEC2_CLAIMED);
    check_msd(OD_MSD_STATUS_SESSION_REQUIRED);
}

int main(void)
{
    test_msd_states();
    test_causes();
    test_history_edges();
    test_gesture_claimed();
    test_five_cycles();
    test_reset_pattern_flag();
    test_session();
    test_pin();
    test_power_cut_everywhere();
    test_marker_codec();
    printf("claim_mode: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
