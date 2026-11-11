/* sec2_test.c -- shared/core/od_sec2 + od_gate + od_reply, driven through od_dispatch_frame().
 *
 * The host side is tests/host/sec2_client.c, which is NOT INDEPENDENT (see its header): same
 * author, same reading of docs/SECURITY_V2.md. The vectors here are therefore self-consistency
 * plus the fixed facts a second reading cannot drift on: the P-256 curve (OpenSSL), RFC 4493
 * CMAC (aes128 suite), the byte layouts copied from the spec's tables.
 *
 * Coverage map (brief): handshake happy path and every failure branch; replay, reflection and
 * reordering; counter exhaustion (OD_SEC2_TEST_CTR_MAX build); tag failures and teardown; rate
 * limiting; fragmentation at MTU 23; storage corruption -> LOCKED; SEC_STATUS; SEC_KEY; the
 * claim with claim mode simulated here (WP3b owns the real one).
 */

#include "od_dispatch.h"

#include "od_cmd_app.h"
#include "od_config_read.h"
#include "od_gate.h"
#include "od_pipe.h"
#include "od_reply.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_sec2_code.h"
#include "od_sec2_kdf.h"
#include "od_xfer.h"
#include "sec2_client.h"
#include "session_fake.h"

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

/* ------------------------------------------------------------------------------ fake radio --- */

#define SENT_MAX 64u
static struct { od_origin_t origin; uint32_t tag; uint16_t len; uint8_t data[256]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static unsigned g_pop;
static unsigned g_radio_budget = 1000u;   /* frames the radio accepts before RETRY */
static unsigned g_tx_total;               /* every notification sent, for fragment counts */
static bool g_radio_flaky;                /* RETRY every other send */
static unsigned g_radio_retries;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    if (g_radio_budget == 0u || (g_radio_flaky && (++g_radio_retries % 2u) == 1u)) {
        return OD_RADIO_RETRY;
    }
    --g_radio_budget;
    ++g_tx_total;
    if (g_pop == g_sent_n) {              /* everything captured so far was consumed */
        g_pop = 0u;
        g_sent_n = 0u;
    }
    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].origin = origin;
        g_sent[g_sent_n].tag = tag;
        g_sent[g_sent_n].len = len;
        memcpy(g_sent[g_sent_n].data, frame, len);
    }
    ++g_sent_n;
    return OD_RADIO_SENT;
}

bool sec2c_radio_pop(uint8_t *buf, uint16_t *len)
{
    if (g_pop >= g_sent_n || g_pop >= SENT_MAX) {
        return false;
    }
    memcpy(buf, g_sent[g_pop].data, g_sent[g_pop].len);
    *len = g_sent[g_pop].len;
    ++g_pop;
    return true;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return true;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
    (void)rp; (void)outcome;
}

bool od_cmd_mutates_config(uint16_t cmd) { (void)cmd; return false; }

/* ------------------------------------------------------------------------- od_sec2_app hooks --- */

static uint32_t g_now_ms = 1000u;
static const struct SecurityConfig *g_app_sec;
static struct od_sec2_claim_mode g_cm;
static unsigned g_ev[5];
static uint32_t g_ev_code;
static bool g_scrub_ok = true;
static unsigned g_rest_syncs;

uint32_t od_sec2_app_now_ms(void) { return g_now_ms; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL | 0xF0u; }
const struct SecurityConfig *od_sec2_app_security(void) { return g_app_sec; }

void od_sec2_app_claim_mode(struct od_sec2_claim_mode *out) { *out = g_cm; }

void od_sec2_app_claim_event(enum od_sec2_claim_event ev, uint32_t arg)
{
    ++g_ev[ev];
    if (ev == OD_SEC2_EV_CODE) {
        g_ev_code = arg;
        if (g_cm.renders_left > 0u) { --g_cm.renders_left; }
        g_cm.config_locked = true;
    }
}

bool od_sec2_app_scrub(void) { return g_scrub_ok; }
void od_sec2_app_rest_sync(void) { ++g_rest_syncs; }

/* --------------------------------------------------------------------------- the fake hooks --- */

static unsigned g_hook_calls;
static uint16_t g_hook_cmd;
static uint8_t  g_hook_body[256];
static uint16_t g_hook_body_n;
static bool     g_hook_sealed;

static od_cmd_result_t record(uint16_t cmd, const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t ack[3];

    ++g_hook_calls;
    g_hook_cmd = cmd;
    g_hook_body_n = (uint16_t)body.n;
    if (body.n != 0u) { memcpy(g_hook_body, body.p, body.n); }
    g_hook_sealed = ctx->was_protected;
    ack[0] = 0x00u;
    ack[1] = (uint8_t)cmd;
    ack[2] = 0x00u;
    (void)od_reply(ctx->r, &ctx->rp, ack, sizeof ack);
    return OD_CMD_OK;
}

#define HOOK(fn, cmd)                                                    \
    od_cmd_result_t fn(const od_cmd_ctx_t *ctx, od_span_t body)          \
    { return record(cmd, ctx, body); }

HOOK(od_cmd_app_reboot,           CMD_REBOOT)
HOOK(od_cmd_app_firmware_version, CMD_FIRMWARE_VERSION)
HOOK(od_cmd_app_read_msd,         CMD_READ_MSD)
HOOK(od_cmd_app_enter_dfu,        CMD_ENTER_DFU)
HOOK(od_cmd_app_power_off,        CMD_POWER_OFF)
HOOK(od_cmd_app_deep_sleep,       CMD_DEEP_SLEEP)
HOOK(od_cmd_app_config_read,      CMD_CONFIG_READ)
HOOK(od_cmd_app_config_write,     CMD_CONFIG_WRITE)
HOOK(od_cmd_app_config_chunk,     CMD_CONFIG_CHUNK)
HOOK(od_cmd_app_config_clear,     CMD_CONFIG_CLEAR)
HOOK(od_xfer_direct_start,        CMD_DIRECT_WRITE_START)
HOOK(od_xfer_data,                CMD_DIRECT_WRITE_DATA)
HOOK(od_xfer_end,                 CMD_DIRECT_WRITE_END)
HOOK(od_xfer_partial_start,       CMD_PARTIAL_WRITE_START)
HOOK(od_pipe_start,               CMD_PIPE_WRITE_START)
HOOK(od_pipe_end,                 CMD_PIPE_WRITE_END)
HOOK(od_cmd_app_led_activate,     CMD_LED_ACTIVATE)
HOOK(od_cmd_app_led_stop,         CMD_LED_STOP)
HOOK(od_cmd_app_buzzer,           CMD_BUZZER)
HOOK(od_nfc_frame,                CMD_NFC_ENDPOINT)

/* PIPE DATA answers nothing here, like a data frame between SACKs. */
od_cmd_result_t od_pipe_data(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx; (void)body;
    ++g_hook_calls;
    g_hook_cmd = CMD_PIPE_WRITE_DATA;
    return OD_CMD_OK;
}

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE = { OD_ORIGIN_BLE, 9u };
static const od_reply_t BLE2 = { OD_ORIGIN_BLE, 10u };
static const od_reply_t LAN = { OD_ORIGIN_LAN_PLAIN, 3u };
static const od_reply_t TLS = { OD_ORIGIN_LAN_TLS, 4u };

static struct sec2c C;

static void clear_capture(void)
{
    g_sent_n = 0u;
    g_pop = 0u;
}

/* A clean device. `claimed` boots it from a 2.x key (row 4): CLAIMED, LEGACY, K_dev = MASTER. */
static void setup(bool claimed)
{
    fake_reset();
    od_txq_reset();
    od_config_read_cancel();
    clear_capture();
    g_now_ms = 1000u;
    g_radio_budget = 1000u;
    memset(&g_cm, 0, sizeof g_cm);
    memset(g_ev, 0, sizeof g_ev);
    g_scrub_ok = true;
    g_rest_syncs = 0u;
    g_hook_calls = 0u;
    sec_init(0u);
    g_app_sec = &g_sec;
    (void)od_sec2_boot(claimed ? &g_sec : NULL);
    clear_capture();
    sec2c_init(&C, MASTER, UID);
}

/* Send one frame; return how many frames came back, the first in f0/n0. */
static uint8_t f0[256];
static uint16_t n0;
static unsigned send(const od_reply_t *rp, const uint8_t *frame, uint16_t n)
{
    uint8_t frames[8][256];
    uint16_t lens[8];
    unsigned got;

    clear_capture();
    g_hook_calls = 0u;
    got = sec2c_send(rp, frame, n, frames, lens, 8u);
    n0 = 0u;
    if (got != 0u) { memcpy(f0, frames[0], lens[0]); n0 = lens[0]; }
    return got;
}

static unsigned send_sealed(uint16_t cmd, const uint8_t *pl, uint16_t n)
{
    uint8_t f[256];
    return send(&BLE, f, sec2c_seal(&C, cmd, pl, n, f));
}

/* Open f0 with the client's session; true when it is a sealed frame equal to `exp`. */
static bool reply_is(const uint8_t *exp, uint16_t n)
{
    uint8_t p[256];
    uint16_t pn = 0u;

    return sec2c_open(&C, f0, n0, p, &pn) && pn == n && memcmp(p, exp, n) == 0;
}

static bool plain_is(const uint8_t *exp, uint16_t n)
{
    return n0 == n && memcmp(f0, exp, n) == 0;
}

/* ---------------------------------------------------------------------------------- cases --- */

static void test_primitives(void)
{
    uint8_t k[16], exp[16];
    char s[8];
    uint8_t qc[64], qd[64], nc[16], nd[16], in[160];
    uint32_t code = 0u;
    unsigned i;

    CASE("kdf matches the client's transcription");
    fake_reset();
    g_force_after = -1;
    CHECK(od_sec2_kdf(MASTER, "OD2/k-ext", UID, 8u, k) == OD_HAL_CRYPTO_OK);
    sec2c_kdf(MASTER, "OD2/k-ext", UID, 8u, exp);
    CHECK(memcmp(k, exp, 16u) == 0);
    CHECK(od_sec2_kdf(MASTER, "OD2/v1-compat-too-long", UID, 8u, k) == OD_HAL_CRYPTO_ERROR);

    CASE("code: input order and truncation");
    for (i = 0; i < 64u; ++i) { qc[i] = (uint8_t)i; qd[i] = (uint8_t)(0x80u + i); }
    memset(nc, 0x11, 16u);
    memset(nd, 0x22, 16u);
    CHECK(od_sec2_code(nc, UID, qc, qd, nd, &code) == OD_HAL_CRYPTO_OK);
    memcpy(in, "OD2/code", 8u);
    memcpy(&in[8], UID, 8u);
    memcpy(&in[16], qc, 64u);
    memcpy(&in[80], qd, 64u);
    memcpy(&in[144], nd, 16u);
    host_cmac(nc, in, 160u, exp);
    CHECK(code == ((((uint32_t)exp[12] << 24) | ((uint32_t)exp[13] << 16) |
                    ((uint32_t)exp[14] << 8) | exp[15]) % 1000000u));
    od_sec2_code_format(123456u, s);
    CHECK(strcmp(s, "123 456") == 0);
    od_sec2_code_format(7u, s);
    CHECK(strcmp(s, "000 007") == 0);

    CASE("commitment input order");
    CHECK(od_sec2_commit(nd, UID, qd, qc, k) == OD_HAL_CRYPTO_OK);
    memcpy(in, "OD2/commit", 10u);
    memcpy(&in[10], UID, 8u);
    memcpy(&in[18], qd, 64u);
    memcpy(&in[82], qc, 64u);
    host_cmac(nd, in, 146u, exp);
    CHECK(memcmp(k, exp, 16u) == 0);
}

static void put_be(uint8_t *p, const char *hex)
{
    unsigned i;
    for (i = 0; i < 32u; ++i) {
        unsigned v;
        (void)sscanf(&hex[2u * i], "%2x", &v);
        p[i] = (uint8_t)v;
    }
}

static void test_point_validation(void)
{
    static const char GX[] = "6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296";
    static const char GY[] = "4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5";
    static const char P[]  = "FFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF";
    uint8_t q[64];
    unsigned i;

    CASE("P-256 public-key validation");
    put_be(q, GX);
    put_be(q + 32, GY);
    CHECK(od_sec2_point_valid(q));
    q[63] ^= 1u;
    CHECK(!od_sec2_point_valid(q));              /* off the curve */
    memset(q, 0, sizeof q);
    CHECK(!od_sec2_point_valid(q));              /* (0, 0) */
    put_be(q, P);
    put_be(q + 32, GY);
    CHECK(!od_sec2_point_valid(q));              /* X == p */
    put_be(q, GX);
    put_be(q + 32, P);
    CHECK(!od_sec2_point_valid(q));              /* Y == p */
    for (i = 0; i < 50u; ++i) {                  /* random OpenSSL keys are all valid */
        void *k = tp256_new(q);
        CHECK(k != NULL && od_sec2_point_valid(q));
        tp256_free(k);
    }
}

static void test_records(void)
{
    struct od_sec2_key_rec k = { OD_SEC2_ORIGIN_CLAIMED, 0x1234u, {0} }, k2;
    struct od_sec2_state_rec s = { OD_SEC2_OS2S_MIGRATED, 2u, 7u }, s2;
    uint8_t r[OD_SEC2_OS2K_LEN], t[OD_SEC2_OS2S_LEN];
    unsigned i;

    CASE("OS2K / OS2S round trip and rejection");
    memcpy(k.key, MASTER, 16u);
    od_sec2_os2k_encode(&k, r);
    CHECK(memcmp(r, "OS2K\x02\x01\x34\x12", 8u) == 0);
    CHECK(od_sec2_os2k_decode(r, &k2) && k2.gen == 0x1234u && memcmp(k2.key, MASTER, 16u) == 0);
    for (i = 0; i < sizeof r; ++i) {
        r[i] ^= 0x40u;
        CHECK(!od_sec2_os2k_decode(r, &k2));      /* any single flipped byte: not valid */
        r[i] ^= 0x40u;
    }
    k.origin = 3u;
    od_sec2_os2k_encode(&k, r);
    CHECK(!od_sec2_os2k_decode(r, &k2));          /* CRC fine, origin out of range */
    od_sec2_os2s_encode(&s, t);
    CHECK(od_sec2_os2s_decode(t, &s2) && s2.key_gen == 7u && s2.short_boots == 2u);
    t[15] ^= 1u;
    CHECK(!od_sec2_os2s_decode(t, &s2));
}

static void put_os2k(uint8_t origin, uint16_t gen, const uint8_t key[16])
{
    struct od_sec2_key_rec k;
    k.origin = origin;
    k.gen = gen;
    memcpy(k.key, key, 16u);
    od_sec2_os2k_encode(&k, g_secret[0].data);
    g_secret[0].len = OD_SEC2_OS2K_LEN;
    g_secret[0].present = true;
}

static void put_os2s(uint16_t key_gen)
{
    struct od_sec2_state_rec s = { 0u, 0u, 0u };
    s.key_gen = key_gen;
    od_sec2_os2s_encode(&s, g_secret[1].data);
    g_secret[1].len = OD_SEC2_OS2S_LEN;
    g_secret[1].present = true;
}

static void test_boot(void)
{
    struct od_sec2_key_rec k;
    struct od_sec2_state_rec st;
    struct SecurityConfig ev;

    CASE("boot row 8: nothing stored");
    fake_reset();
    CHECK(od_sec2_boot(NULL) == OD_SEC2_UNCLAIMED);
    CHECK(!od_sec2_needs_session());

    CASE("boot row 4: a 2.x key migrates, OS2S first");
    fake_reset();
    sec_init(0u);
    g_secret_fail_after = 1;                      /* OS2S lands, the cut comes before OS2K */
    CHECK(od_sec2_boot(&g_sec) == OD_SEC2_CLAIMED);
    CHECK(g_secret[1].present && !g_secret[0].present);
    CHECK(od_sec2_boot(&g_sec) == OD_SEC2_CLAIMED);   /* re-runs row 4, not row 1 */
    g_secret_fail_after = -1;
    CHECK(od_sec2_boot(&g_sec) == OD_SEC2_CLAIMED);
    CHECK(od_sec2_os2k_decode(g_secret[0].data, &k) && k.origin == OD_SEC2_ORIGIN_LEGACY &&
          k.gen == 1u && memcmp(k.key, MASTER, 16u) == 0);
    CHECK(od_sec2_os2s_decode(g_secret[1].data, &st) && st.key_gen == 1u);

    CASE("boot row 2: a valid OS2K wins over the 0x27 bytes");
    {
        uint8_t other[16];
        memset(other, 0x77, sizeof other);
        fake_reset();
        put_os2k(OD_SEC2_ORIGIN_CLAIMED, 3u, other);
        put_os2s(3u);
        sec_init(0u);
        CHECK(od_sec2_boot(&g_sec) == OD_SEC2_CLAIMED);
        CHECK(od_sec2_k_v1(k.key) && memcmp(k.key, MASTER, 16u) != 0);
    }

    CASE("boot row 2 repairs a missing OS2S");
    fake_reset();
    put_os2k(OD_SEC2_ORIGIN_CLAIMED, 5u, MASTER);
    CHECK(od_sec2_boot(NULL) == OD_SEC2_CLAIMED);
    CHECK(od_sec2_os2s_decode(g_secret[1].data, &st) && st.key_gen == 5u);

    CASE("boot row 1: an older key came back -> LOCKED");
    fake_reset();
    put_os2k(OD_SEC2_ORIGIN_CLAIMED, 2u, MASTER);
    put_os2s(3u);
    CHECK(od_sec2_boot(NULL) == OD_SEC2_LOCKED);
    CHECK(od_sec2_needs_session());

    CASE("boot row 3: storage corruption with claim evidence -> LOCKED");
    fake_reset();
    put_os2k(OD_SEC2_ORIGIN_CLAIMED, 1u, MASTER);
    g_secret[0].data[10] ^= 1u;                   /* CRC now fails */
    memset(&ev, 0, sizeof ev);
    ev.reserved[0] = OD_SEC27_AT_REST_SEC2;
    CHECK(od_sec2_boot(&ev) == OD_SEC2_LOCKED);

    CASE("corrupt OS2K without evidence and without a 2.x key -> UNCLAIMED");
    fake_reset();
    put_os2k(OD_SEC2_ORIGIN_CLAIMED, 1u, MASTER);
    g_secret[0].data[0] = 'X';
    CHECK(od_sec2_boot(NULL) == OD_SEC2_UNCLAIMED);

    CASE("a zero 2.x key is no key");
    fake_reset();
    sec_init(0u);
    memset(g_sec.encryption_key, 0, 16u);
    CHECK(od_sec2_boot(&g_sec) == OD_SEC2_UNCLAIMED);
}

static void test_auth_happy(void)
{
    uint8_t ack[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0x00u };
    uint8_t body[4] = { 1, 2, 3, 4 };
    unsigned frames;

    CASE("auth happy path at MTU 23");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    CHECK(od_sec2_session_live(&BLE));
    CHECK(!od_sec2_session_live(&BLE2));
    CHECK(C.timeout_s == 3600u);                  /* 0 means 3600 on 3.0 */
    CHECK((C.dflags & OD_SEC2_DFLAG_KEY_LEGACY) != 0u);
    CHECK((C.dflags & 0xF0u) == 0u);              /* target noise above bit 1 masked off */
    CHECK(!g_ecdh_live);                          /* the ephemeral key is gone */

    CASE("a sealed command runs and its reply is sealed");
    frames = send_sealed(CMD_REBOOT, body, sizeof body);
    CHECK(frames == 1u && g_hook_calls == 1u && g_hook_sealed);
    CHECK(g_hook_body_n == 4u && memcmp(g_hook_body, body, 4u) == 0);
    CHECK(reply_is(ack, sizeof ack));

    CASE("a sealed command with an empty payload");
    frames = send_sealed(CMD_REBOOT, NULL, 0u);
    CHECK(frames == 1u && g_hook_calls == 1u && g_hook_body_n == 0u);

    CASE("fragment count: HELLO at frag_max 20 is 6 writes, its reply 6 notifications");
    {
        uint8_t b[82] = {0}, r[96];
        int n;
        void *k = tp256_new(&b[2]);
        b[0] = 2u;
        b[1] = 20u;
        g_tx_total = 0u;
        n = sec2c_xchg(&C, &BLE, CMD_SEC_AUTH, OD_SEC2_AUTH_HELLO, b, 82u, r, sizeof r);
        CHECK(n == 81);
        CHECK(g_tx_total == 6u);          /* 81 B at 16 B a notification */
        tp256_free(k);
    }

    CASE("frag_max 244: one fragment each way");
    setup(true);
    C.frag_max = 244u;
    CHECK(sec2c_auth(&C, &BLE) == 0);

    CASE("timeout 1..59 is raised to 60");
    setup(true);
    g_sec.session_timeout_seconds = 5u;
    CHECK(sec2c_auth(&C, &BLE) == 0 && C.timeout_s == 60u);

    CASE("session lifetime: FE once it runs out");
    g_now_ms += 60000u;
    CHECK(send_sealed(CMD_REBOOT, NULL, 0u) == 1u);
    {
        const uint8_t fe[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0xFEu };
        CHECK(plain_is(fe, 3u) && g_hook_calls == 0u);
    }
}

static int hello_with(const uint8_t *qc, uint8_t ver, uint8_t frag_max)
{
    uint8_t b[82], r[96];
    int n;

    b[0] = ver;
    b[1] = frag_max;
    memcpy(&b[2], qc, 64u);
    memset(&b[66], 0x55, 16u);
    n = sec2c_xchg(&C, &BLE, CMD_SEC_AUTH, OD_SEC2_AUTH_HELLO, b, 82u, r, sizeof r);
    return n;
}

static void test_auth_failures(void)
{
    uint8_t qc[64], tag[16];
    void *k;

    CASE("wrong key: AUTH_FAILED, no session");
    setup(true);
    memset(C.k_dev, 0x99, 16u);
    CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_AUTH_FAILED);
    CHECK(!od_sec2_session_live(NULL));
    CHECK(!fake_slot_loaded(0u) && !fake_slot_loaded(1u));

    CASE("unclaimed: NOT_CLAIMED");
    setup(false);
    CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_NOT_CLAIMED);

    CASE("LOCKED: LOCKED");
    fake_reset();
    put_os2k(OD_SEC2_ORIGIN_CLAIMED, 1u, MASTER);
    put_os2s(2u);
    (void)od_sec2_boot(NULL);
    od_txq_reset();
    clear_capture();
    CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_LOCKED);

    CASE("LAN: WRONG_TRANSPORT");
    setup(true);
    CHECK(sec2c_auth(&C, &LAN) == OD_SEC_ERR_WRONG_TRANSPORT);
    CHECK(sec2c_auth(&C, &TLS) == OD_SEC_ERR_WRONG_TRANSPORT);

    CASE("HELLO content: version, frag_max, point");
    setup(true);
    k = tp256_new(qc);
    CHECK(hello_with(qc, 1u, 20u) == -(int)OD_SEC_ERR_MALFORMED);
    CHECK(hello_with(qc, 2u, 19u) == -(int)OD_SEC_ERR_MALFORMED);
    CHECK(hello_with(qc, 2u, 245u) == -(int)OD_SEC_ERR_MALFORMED);
    qc[5] ^= 1u;
    CHECK(hello_with(qc, 2u, 20u) == -(int)OD_SEC_ERR_BAD_POINT);
    CHECK(!g_ecdh_live);
    tp256_free(k);

    CASE("FINISH without HELLO: BAD_STATE");
    setup(true);
    memset(tag, 0, 16u);
    CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_AUTH, OD_SEC2_AUTH_FINISH, tag, 16u, qc, 64u) ==
          -(int)OD_SEC_ERR_BAD_STATE);

    CASE("FINISH from another connection: BAD_STATE");
    setup(true);
    k = tp256_new(qc);
    CHECK(hello_with(qc, 2u, 20u) == 81);
    CHECK(sec2c_xchg(&C, &BLE2, CMD_SEC_AUTH, OD_SEC2_AUTH_FINISH, tag, 16u, qc, 64u) ==
          -(int)OD_SEC_ERR_BAD_STATE);
    tp256_free(k);

    CASE("FINISH after 30 s: EXPIRED");
    setup(true);
    k = tp256_new(qc);
    CHECK(hello_with(qc, 2u, 20u) == 81);
    g_now_ms += OD_SEC2_HELLO_TO_FINISH_MS + 1u;
    CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_AUTH, OD_SEC2_AUTH_FINISH, tag, 16u, qc, 64u) ==
          -(int)OD_SEC_ERR_EXPIRED);
    CHECK(!fake_slot_loaded(0u));
    tp256_free(k);

    CASE("unknown sub: MALFORMED");
    setup(true);
    CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_AUTH, 0x07u, tag, 16u, qc, 64u) ==
          -(int)OD_SEC_ERR_MALFORMED);

    CASE("every engine failure during HELLO is CRYPTO_ERROR and leaves no key behind");
    {
        int32_t at;
        for (at = 0; at < 14; ++at) {
            setup(true);
            g_force_status = OD_HAL_CRYPTO_ERROR;
            g_force_after = at;
            g_hal_calls = 0u;
            {
                const int rc = sec2c_auth(&C, &BLE);
                CHECK(rc == OD_SEC_ERR_CRYPTO_ERROR || rc == 0);
            }
            CHECK(!g_ecdh_live);
            if (!od_sec2_session_live(NULL)) {
                CHECK(!fake_slot_loaded(0u) && !fake_slot_loaded(1u));
            }
        }
    }

    CASE("a HELLO tears the live session down");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    k = tp256_new(qc);
    CHECK(hello_with(qc, 2u, 20u) == 81);
    CHECK(!od_sec2_session_live(NULL));
    tp256_free(k);
}

static void test_rate_limit(void)
{
    unsigned i;

    CASE("rate limiter: 10 starts per 60 s");
    setup(true);
    for (i = 0; i < OD_SEC2_RATE_EVENTS; ++i) {
        CHECK(sec2c_auth(&C, &BLE) == 0);
        g_now_ms += 100u;
    }
    CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_RATE_LIMITED);
    g_now_ms += OD_SEC2_RATE_WINDOW_MS;
    CHECK(sec2c_auth(&C, &BLE) == 0);

    CASE("a failed FINISH counts as well");
    setup(true);
    memset(C.k_dev, 0x99, 16u);
    for (i = 0; i < 5u; ++i) {
        CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_AUTH_FAILED);   /* HELLO + bad FINISH: 2 each */
    }
    CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_RATE_LIMITED);
}

static void frag(uint8_t sub, uint8_t fb, const uint8_t *chunk, uint8_t n)
{
    uint8_t f[64];
    f[0] = 0x00u;
    f[1] = (uint8_t)CMD_SEC_AUTH;
    f[2] = sub;
    f[3] = fb;
    if (n != 0u) { memcpy(&f[4], chunk, n); }
    (void)send(&BLE, f, (uint16_t)(4u + n));
}

static void test_fragments(void)
{
    uint8_t z[16] = {0};
    const uint8_t mal[4] = { 0xFFu, (uint8_t)CMD_SEC_AUTH, OD_SEC2_AUTH_HELLO, OD_SEC_ERR_MALFORMED };

    CASE("a non-LAST fragment draws no notification");
    setup(true);
    frag(OD_SEC2_AUTH_HELLO, 0x00u, z, 16u);
    CHECK(g_sent_n == 0u);

    CASE("out-of-order index: MALFORMED");
    frag(OD_SEC2_AUTH_HELLO, 0x02u, z, 16u);
    CHECK(plain_is(mal, 4u));

    CASE("first fragment not index 0: MALFORMED");
    frag(OD_SEC2_AUTH_HELLO, 0x01u, z, 16u);
    CHECK(plain_is(mal, 4u));

    CASE("sub changes mid-message: MALFORMED");
    frag(OD_SEC2_AUTH_HELLO, 0x00u, z, 16u);
    frag(OD_SEC2_AUTH_FINISH, 0x01u, z, 16u);
    CHECK(g_sent_n == 1u && f0[0] == 0xFFu && f0[3] == OD_SEC_ERR_MALFORMED);

    CASE("empty non-LAST fragment: MALFORMED");
    frag(OD_SEC2_AUTH_HELLO, 0x00u, z, 0u);
    CHECK(plain_is(mal, 4u));

    CASE("a body past its fixed length: MALFORMED");
    frag(OD_SEC2_AUTH_FINISH, 0x00u, z, 16u);
    {
        const uint8_t m2[4] = { 0xFFu, (uint8_t)CMD_SEC_AUTH, OD_SEC2_AUTH_FINISH, OD_SEC_ERR_MALFORMED };
        frag(OD_SEC2_AUTH_FINISH, 0x81u, z, 1u);
        CHECK(plain_is(m2, 4u));
    }

    CASE("LAST leaves the body short: MALFORMED");
    frag(OD_SEC2_AUTH_HELLO, 0x80u, z, 16u);
    CHECK(plain_is(mal, 4u));

    CASE("a gap over 5 s discards silently");
    frag(OD_SEC2_AUTH_HELLO, 0x00u, z, 16u);
    g_now_ms += OD_SEC2_FRAGMENT_GAP_MS + 1u;
    frag(OD_SEC2_AUTH_HELLO, 0x01u, z, 16u);
    CHECK(plain_is(mal, 4u));                     /* index 1 with nothing open */

    CASE("a frag_max below 20 is refused even when the fragments themselves arrive whole");
    setup(true);
    C.frag_max = 5u;                              /* 1-byte H->D chunks: 82 writes */
    CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_MALFORMED);

    CASE("too short to carry sub and frag");
    {
        uint8_t f[3] = { 0x00u, (uint8_t)CMD_SEC_AUTH, 0x01u };
        const uint8_t m3[4] = { 0xFFu, (uint8_t)CMD_SEC_AUTH, 0x01u, OD_SEC_ERR_MALFORMED };
        (void)send(&BLE, f, 3u);
        CHECK(plain_is(m3, 4u));
        (void)send(&BLE, f, 2u);
        CHECK(n0 == 4u && f0[2] == 0x00u);        /* no sub: reported as 0 (A18.5) */
    }

    CASE("D->H fragments under TX back-pressure: every other notification is refused");
    setup(true);
    g_radio_flaky = true;
    CHECK(sec2c_auth(&C, &BLE) == 0);
    CHECK(g_radio_retries >= 6u);
    g_radio_flaky = false;
}

static void test_records_layer(void)
{
    uint8_t f[256], g[256];
    uint16_t n;
    const uint8_t f2[3] = { 0xFFu, (uint8_t)CMD_REBOOT, OD_SEC_NACK_REPLAY };
    const uint8_t f3[3] = { 0xFFu, (uint8_t)CMD_REBOOT, OD_SEC_NACK_INTEGRITY };
    const uint8_t f1[3] = { 0xFFu, (uint8_t)CMD_REBOOT, OD_SEC_NACK_RECORD };
    const uint8_t fe[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0xFEu };
    const uint8_t ok[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0x00u };

    CASE("replay: the same record twice -> F2, sealed, no strike");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    n = sec2c_seal(&C, CMD_REBOOT, NULL, 0u, f);
    CHECK(send(&BLE, f, n) == 1u && reply_is(ok, 3u));
    CHECK(send(&BLE, f, n) == 1u && reply_is(f2, 3u) && g_hook_calls == 0u);

    CASE("counter 0 and 0xFFFFFFFF -> F2");
    CHECK(send(&BLE, g, sec2c_seal_ctr(&C, CMD_REBOOT, 0u, NULL, 0u, g)) == 1u && reply_is(f2, 3u));
    CHECK(send(&BLE, g, sec2c_seal_ctr(&C, CMD_REBOOT, 0xFFFFFFFFu, NULL, 0u, g)) == 1u &&
          reply_is(f2, 3u));

    CASE("reordering inside the window is accepted once");
    C.tx_ctr = 10u;
    n = sec2c_seal_ctr(&C, CMD_REBOOT, 9u, NULL, 0u, g);
    CHECK(send_sealed(CMD_REBOOT, NULL, 0u) == 1u && reply_is(ok, 3u));     /* ctr 11 */
    CHECK(send(&BLE, g, n) == 1u && reply_is(ok, 3u) && g_hook_calls == 1u); /* 9, late */
    CHECK(send(&BLE, g, n) == 1u && reply_is(f2, 3u));

    CASE("a record shorter than 18 bytes -> F1, no strike");
    {
        uint8_t s[6] = { 0x00u, (uint8_t)CMD_REBOOT, 1, 0, 0, 0 };
        CHECK(send(&BLE, s, sizeof s) == 1u && reply_is(f1, 3u));
    }

    CASE("reflection: the device's own D->H record sent back is rejected");
    setup(true);                                  /* fresh: device ctr 1 is unused H->D */
    CHECK(sec2c_auth(&C, &BLE) == 0);
    C.tx_ctr = 1u;
    n = sec2c_seal(&C, CMD_REBOOT, NULL, 0u, f);
    CHECK(send(&BLE, f, n) == 1u);
    memcpy(g, f0, n0);
    {
        uint8_t r[256];
        r[0] = 0x00u;
        r[1] = (uint8_t)CMD_REBOOT;
        memcpy(&r[2], &g[1], (size_t)(n0 - 1u));  /* ctr, C, T of the device's frame */
        CHECK(send(&BLE, r, (uint16_t)(n0 + 1u)) == 1u && reply_is(f3, 3u));
    }

    CASE("an opcode change is caught by the AAD");
    n = sec2c_seal(&C, CMD_REBOOT, NULL, 0u, f);
    f[1] = (uint8_t)CMD_LED_STOP;
    CHECK(send(&BLE, f, n) == 1u);
    {
        const uint8_t f3l[3] = { 0xFFu, (uint8_t)CMD_LED_STOP, OD_SEC_NACK_INTEGRITY };
        CHECK(reply_is(f3l, 3u));
    }

    CASE("third bad tag: F3, then teardown, then FE");
    n = sec2c_seal(&C, CMD_REBOOT, NULL, 0u, f);
    f[n - 1u] ^= 1u;
    CHECK(send(&BLE, f, n) == 1u && reply_is(f3, 3u));     /* strike 3 (reflection, AAD, this) */
    CHECK(!od_sec2_session_live(NULL));
    CHECK(!fake_slot_loaded(0u) && !fake_slot_loaded(1u));
    CHECK(send_sealed(CMD_REBOOT, NULL, 0u) == 1u && plain_is(fe, 3u));

    CASE("no-ack frames: PIPE DATA replay and bad tag are silent, the strike still counts");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    {
        uint8_t d[2] = { 0u, 0u };
        n = sec2c_seal(&C, CMD_PIPE_WRITE_DATA, d, 2u, f);
        CHECK(send(&BLE, f, n) == 0u && g_hook_calls == 1u);
        CHECK(send(&BLE, f, n) == 0u && g_hook_calls == 0u);
        f[n - 1u] ^= 1u;
        f[2] = 0x55u;
        CHECK(send(&BLE, f, n) == 0u);
        CHECK(send(&BLE, f, n) == 0u);
        CHECK(send(&BLE, f, n) == 0u);
        CHECK(!od_sec2_session_live(NULL));
    }

    CASE("an engine fault on open answers [FF][cmd][00] sealed and counts nothing");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    n = sec2c_seal(&C, CMD_REBOOT, NULL, 0u, f);
    g_force_status = OD_HAL_CRYPTO_ERROR;
    g_force_once = true;
    g_force_after = (int32_t)g_hal_calls;         /* the next call: the decrypt */
    CHECK(send(&BLE, f, n) == 1u);
    g_force_status = OD_HAL_CRYPTO_OK;
    {
        const uint8_t e[3] = { 0xFFu, (uint8_t)CMD_REBOOT, 0x00u };
        CHECK(reply_is(e, 3u));
    }
    CHECK(od_sec2_session_live(NULL));

    CASE("a plaintext gated frame on a claimed device: FE");
    setup(true);
    {
        uint8_t p[2] = { 0x00u, (uint8_t)CMD_REBOOT };
        CHECK(send(&BLE, p, 2u) == 1u && plain_is(fe, 3u) && g_hook_calls == 0u);
    }

    CASE("a bad tag does not spend its counter: the genuine record still opens");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    n = sec2c_seal(&C, CMD_REBOOT, NULL, 0u, f);
    memcpy(g, f, n);
    g[n - 1u] ^= 1u;
    CHECK(send(&BLE, g, n) == 1u && reply_is(f3, 3u) && g_hook_calls == 0u);
    CHECK(send(&BLE, f, n) == 1u && reply_is(ok, 3u) && g_hook_calls == 1u);

    CASE("disconnect drops the session and its keys");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    od_sec2_disconnect();
    CHECK(!od_sec2_session_live(NULL) && !fake_slot_loaded(0u) && !fake_slot_loaded(1u));
}

static void test_plaintext_set(void)
{
    uint8_t f[256];
    uint16_t n;

    CASE("AUTHENTICATE v1: NOT_CONFIG unclaimed, V2_REQUIRED claimed");
    setup(false);
    {
        uint8_t a[3] = { 0x00u, 0x50u, 0x00u };
        const uint8_t e3[3] = { 0x00u, 0x50u, AUTH_STATUS_NOT_CONFIG };
        const uint8_t e5[3] = { 0x00u, 0x50u, AUTH_STATUS_V2_REQUIRED };
        CHECK(send(&BLE, a, 3u) == 1u && plain_is(e3, 3u));
        setup(true);
        CHECK(send(&BLE, a, 3u) == 1u && plain_is(e5, 3u));
        CHECK(sec2c_auth(&C, &BLE) == 0);
        CHECK(send(&BLE, a, 3u) == 1u && plain_is(e5, 3u) && od_sec2_session_live(NULL));
    }

    CASE("FIRMWARE_VERSION / READ_MSD stay plaintext inside a session");
    {
        uint8_t v[2] = { 0x00u, (uint8_t)CMD_FIRMWARE_VERSION };
        const uint8_t r[3] = { 0x00u, (uint8_t)CMD_FIRMWARE_VERSION, 0x00u };
        CHECK(send(&BLE, v, 2u) == 1u && plain_is(r, 3u));
        v[1] = (uint8_t)CMD_READ_MSD;
        CHECK(send(&BLE, v, 2u) == 1u && n0 == 3u && f0[1] == (uint8_t)CMD_READ_MSD);
    }

    CASE("a sealed plaintext-set opcode is parsed as plaintext (rule 1)");
    n = sec2c_seal(&C, CMD_FIRMWARE_VERSION, NULL, 0u, f);
    CHECK(send(&BLE, f, n) == 1u && f0[0] == 0x00u && g_hook_calls == 1u && !g_hook_sealed);

    CASE("SEC_STATUS bytes on a claimed, legacy device with a session");
    {
        uint8_t s[2] = { 0x00u, (uint8_t)CMD_SEC_STATUS };
        CHECK(send(&BLE, s, 2u) == 1u && n0 == OD_SEC2_STATUS_LEN);
        CHECK(f0[0] == 0x00u && f0[1] == 0x54u && f0[2] == 2u && f0[3] == OD_SEC2_STATE_CLAIMED);
        CHECK(f0[4] == (OD_SEC2_SFLAG_SESSION_ACTIVE | OD_SEC2_SFLAG_ROTATION_SUGGESTED));
        CHECK(f0[5] == 0u && f0[6] == 0u && memcmp(&f0[7], UID, 8u) == 0);
        CHECK(f0[15] == (3600u & 0xFFu) && f0[16] == (3600u >> 8));
        CHECK(f0[17] == (OD_SEC2_DFLAG_HAS_PANEL | OD_SEC2_DFLAG_KEY_LEGACY) && f0[18] == 0u);
        (void)send(&BLE2, s, 2u);                 /* another connection: no session there */
        CHECK((f0[4] & OD_SEC2_SFLAG_SESSION_ACTIVE) == 0u);
    }

    CASE("LAN: plain is FE when claimed, TLS runs the command");
    setup(true);
    {
        uint8_t p[2] = { 0x00u, (uint8_t)CMD_REBOOT };
        const uint8_t fe[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0xFEu };
        const uint8_t ok[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0x00u };
        CHECK(send(&LAN, p, 2u) == 1u && plain_is(fe, 3u));
        CHECK(send(&TLS, p, 2u) == 1u && plain_is(ok, 3u) && g_hook_calls == 1u);
    }

    CASE("unclaimed: everything is open, nothing sealed");
    setup(false);
    {
        uint8_t p[2] = { 0x00u, (uint8_t)CMD_LED_STOP };
        const uint8_t ok[3] = { 0x00u, (uint8_t)CMD_LED_STOP, 0x00u };
        CHECK(send(&BLE, p, 2u) == 1u && plain_is(ok, 3u));
    }

    CASE("unclaimed SEC_KEY: NOT_CLAIMED, plaintext");
    {
        uint8_t p[3] = { 0x00u, (uint8_t)CMD_SEC_KEY, OD_SEC2_KEY_ROTATE };
        CHECK(send(&BLE, p, 3u) == 1u && n0 == 4u && f0[0] == 0xFFu && f0[3] == OD_SEC_ERR_NOT_CLAIMED);
    }

    CASE("LOCKED: FE for gated commands, SEC_STATUS says LOCKED");
    fake_reset();
    put_os2k(OD_SEC2_ORIGIN_CLAIMED, 1u, MASTER);
    put_os2s(9u);
    (void)od_sec2_boot(NULL);
    od_txq_reset();
    {
        uint8_t p[2] = { 0x00u, (uint8_t)CMD_CONFIG_READ };
        uint8_t s[2] = { 0x00u, (uint8_t)CMD_SEC_STATUS };
        CHECK(send(&TLS, p, 2u) == 1u && f0[2] == 0xFEu);
        CHECK(send(&BLE, s, 2u) == 1u && f0[3] == OD_SEC2_STATE_LOCKED);
    }
}

static void test_claim(void)
{
    struct od_sec2_key_rec k;
    struct od_sec2_state_rec st;
    const uint8_t ok[3] = { 0x00u, (uint8_t)CMD_REBOOT, 0x00u };

    CASE("not in claim mode: NOT_IN_CLAIM_MODE");
    setup(false);
    CHECK(sec2c_claim(&C, &BLE, false) == OD_SEC_ERR_NOT_IN_CLAIM_MODE);

    CASE("claimed: ALREADY_CLAIMED");
    setup(true);
    g_cm.active = true;
    g_cm.renders_left = 3u;
    CHECK(sec2c_claim(&C, &BLE, false) == OD_SEC_ERR_ALREADY_CLAIMED);

    CASE("claim happy path");
    setup(false);
    g_cm.active = true;
    g_cm.fresh = true;
    g_cm.renders_left = 3u;
    CHECK(sec2c_claim(&C, &BLE, false) == 0);
    CHECK(od_sec2_state() == OD_SEC2_CLAIMED && od_sec2_session_live(&BLE));
    CHECK(g_ev[OD_SEC2_EV_CODE] == 1u && g_ev_code == C.code);
    CHECK(g_ev[OD_SEC2_EV_CLAIMED] == 1u && g_rest_syncs == 1u);
    CHECK((C.dflags & OD_SEC2_DFLAG_CLAIM_MODE_FRESH) != 0u);
    CHECK((C.dflags & OD_SEC2_DFLAG_KEY_LEGACY) == 0u);
    CHECK(od_sec2_os2k_decode(g_secret[0].data, &k) && k.origin == OD_SEC2_ORIGIN_CLAIMED &&
          k.gen == 1u && memcmp(k.key, C.k_dev, 16u) == 0);
    CHECK(od_sec2_os2s_decode(g_secret[1].data, &st) && st.key_gen == 1u);
    CHECK(send_sealed(CMD_REBOOT, NULL, 0u) == 1u && reply_is(ok, 3u));

    CASE("after the claim, the new key authenticates");
    od_sec2_disconnect();
    CHECK(sec2c_auth(&C, &BLE) == 0);

    CASE("claim-mode refusal comes back sealed after the claim (C/s in CM)");
    {
        const uint8_t fd[3] = { 0xFFu, (uint8_t)CMD_LED_STOP, OD_GATE_CLAIM_MODE };
        CHECK(send_sealed(CMD_LED_STOP, NULL, 0u) == 1u && reply_is(fd, 3u) && g_hook_calls == 0u);
    }

    CASE("wrong confirmation tag: AUTH_FAILED, still unclaimed, one render used");
    setup(false);
    g_cm.active = true;
    g_cm.renders_left = 3u;
    CHECK(sec2c_claim(&C, &BLE, true) == OD_SEC_ERR_AUTH_FAILED);
    CHECK(od_sec2_state() == OD_SEC2_UNCLAIMED && !g_secret[0].present);
    CHECK(g_ev[OD_SEC2_EV_DISCARDED] == 1u && g_cm.renders_left == 2u);

    CASE("the third failed render answers ATTEMPTS_EXHAUSTED");
    CHECK(sec2c_claim(&C, &BLE, true) == OD_SEC_ERR_AUTH_FAILED);
    CHECK(sec2c_claim(&C, &BLE, true) == OD_SEC_ERR_ATTEMPTS_EXHAUSTED);
    CHECK(g_ev[OD_SEC2_EV_EXHAUSTED] == 1u);

    CASE("storage failure at CONFIRM: STORAGE_ERROR, unclaimed");
    setup(false);
    g_cm.active = true;
    g_cm.renders_left = 3u;
    g_secret_fail_after = 0;
    CHECK(sec2c_claim(&C, &BLE, false) == OD_SEC_ERR_STORAGE_ERROR);
    CHECK(od_sec2_state() == OD_SEC2_UNCLAIMED);

    CASE("NONCE without BEGIN: BAD_STATE; NONCE after 30 s: EXPIRED");
    setup(false);
    g_cm.active = true;
    g_cm.renders_left = 3u;
    {
        uint8_t nc[16] = {0}, r[96], b[66];
        void *key = tp256_new(&b[2]);
        CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_CLAIM, OD_SEC2_CLAIM_NONCE, nc, 16u, r, 96u) ==
              -(int)OD_SEC_ERR_BAD_STATE);
        b[0] = 2u;
        b[1] = 20u;
        CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_CLAIM, OD_SEC2_CLAIM_BEGIN, b, 66u, r, 96u) == 81);
        g_now_ms += OD_SEC2_BEGIN_TO_NONCE_MS + 1u;
        CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_CLAIM, OD_SEC2_CLAIM_NONCE, nc, 16u, r, 96u) ==
              -(int)OD_SEC_ERR_EXPIRED);
        CHECK(!g_ecdh_live);
        tp256_free(key);
    }

    CASE("ABORT and EXIT: empty replies");
    {
        uint8_t r[8];
        CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_CLAIM, OD_SEC2_CLAIM_ABORT, NULL, 0u, r, 8u) == 0);
        CHECK(sec2c_xchg(&C, &BLE, CMD_SEC_CLAIM, OD_SEC2_CLAIM_EXIT, NULL, 0u, r, 8u) == 0);
        CHECK(g_ev[OD_SEC2_EV_EXIT] == 1u);
    }

    CASE("claim mode allow-list (unclaimed): FD in plaintext, config writes while FRESH");
    setup(false);
    g_cm.active = true;
    g_cm.fresh = true;
    g_cm.renders_left = 3u;
    {
        uint8_t p[2] = { 0x00u, (uint8_t)CMD_LED_STOP };
        const uint8_t fd[3] = { 0xFFu, (uint8_t)CMD_LED_STOP, OD_GATE_CLAIM_MODE };
        CHECK(send(&BLE, p, 2u) == 1u && plain_is(fd, 3u) && g_hook_calls == 0u);
        p[1] = (uint8_t)CMD_CONFIG_WRITE;
        CHECK(send(&BLE, p, 2u) == 1u && g_hook_calls == 1u);
        g_cm.config_locked = true;
        CHECK(send(&BLE, p, 2u) == 1u && f0[2] == OD_GATE_CLAIM_MODE && g_hook_calls == 0u);
        p[1] = (uint8_t)CMD_CONFIG_READ;
        CHECK(send(&BLE, p, 2u) == 1u && g_hook_calls == 1u);
        p[1] = (uint8_t)CMD_SEC_KEY;
        CHECK(send(&BLE, p, 2u) == 1u && f0[2] == OD_GATE_CLAIM_MODE);
    }
}

static void test_sec_key(void)
{
    uint8_t p[17], r[256], ctx[24], kconf[16], knew[16];
    uint16_t rn;
    struct od_sec2_key_rec k;

    CASE("ROTATE then ROTATE_CONFIRM: new key persisted, session torn down after the reply");
    setup(true);
    CHECK(sec2c_auth(&C, &BLE) == 0);
    p[0] = OD_SEC2_KEY_ROTATE;
    CHECK(send_sealed(CMD_SEC_KEY, p, 1u) == 1u);
    CHECK(sec2c_open(&C, f0, n0, r, &rn) && rn == 19u && r[0] == 0x00u && r[1] == 0x57u &&
          r[2] == OD_SEC2_KEY_ROTATE);
    memcpy(knew, &r[3], 16u);
    memcpy(ctx, UID, 8u);
    memcpy(&ctx[8], C.sid, 16u);
    sec2c_kdf(knew, "OD2/k-conf", UID, 8u, kconf);
    p[0] = OD_SEC2_KEY_ROTATE_CONFIRM;
    sec2c_kdf(kconf, "OD2/rotate-c", ctx, 24u, &p[1]);
    CHECK(send_sealed(CMD_SEC_KEY, p, 17u) == 1u);
    CHECK(sec2c_open(&C, f0, n0, r, &rn) && rn == 3u && r[0] == 0x00u);
    CHECK(!od_sec2_session_live(NULL));
    CHECK(od_sec2_os2k_decode(g_secret[0].data, &k) && k.gen == 2u && memcmp(k.key, knew, 16u) == 0 &&
          k.origin == OD_SEC2_ORIGIN_CLAIMED);
    CHECK(sec2c_auth(&C, &BLE) == OD_SEC_ERR_AUTH_FAILED);   /* the old key no longer works */
    memcpy(C.k_dev, knew, 16u);
    g_now_ms += OD_SEC2_RATE_WINDOW_MS;
    CHECK(sec2c_auth(&C, &BLE) == 0);

    CASE("ROTATE_CONFIRM with a wrong tag: AUTH_FAILED, old key kept");
    p[0] = OD_SEC2_KEY_ROTATE;
    CHECK(send_sealed(CMD_SEC_KEY, p, 1u) == 1u);
    p[0] = OD_SEC2_KEY_ROTATE_CONFIRM;
    memset(&p[1], 0, 16u);
    CHECK(send_sealed(CMD_SEC_KEY, p, 17u) == 1u);
    CHECK(sec2c_open(&C, f0, n0, r, &rn) && rn == 4u && r[0] == 0xFFu && r[3] == OD_SEC_ERR_AUTH_FAILED);
    CHECK(od_sec2_session_live(NULL));

    CASE("ROTATE_CONFIRM without ROTATE: BAD_STATE");
    CHECK(send_sealed(CMD_SEC_KEY, p, 17u) == 1u);
    CHECK(sec2c_open(&C, f0, n0, r, &rn) && rn == 4u && r[3] == OD_SEC_ERR_BAD_STATE);

    CASE("UNCLAIM: scrub refused -> STORAGE_ERROR, still claimed");
    p[0] = OD_SEC2_KEY_UNCLAIM;
    p[1] = 0xA5u;
    p[2] = 0x5Au;
    g_scrub_ok = false;
    CHECK(send_sealed(CMD_SEC_KEY, p, 3u) == 1u);
    CHECK(sec2c_open(&C, f0, n0, r, &rn) && rn == 4u && r[3] == OD_SEC_ERR_STORAGE_ERROR);
    CHECK(od_sec2_state() == OD_SEC2_CLAIMED);

    CASE("UNCLAIM without the guard bytes: MALFORMED");
    p[1] = 0x00u;
    g_scrub_ok = true;
    CHECK(send_sealed(CMD_SEC_KEY, p, 3u) == 1u);
    CHECK(sec2c_open(&C, f0, n0, r, &rn) && rn == 4u && r[3] == OD_SEC_ERR_MALFORMED);

    CASE("UNCLAIM: OS2K erased, tombstone written, open afterwards");
    p[1] = 0xA5u;
    CHECK(send_sealed(CMD_SEC_KEY, p, 3u) == 1u);
    CHECK(sec2c_open(&C, f0, n0, r, &rn) && rn == 3u && r[0] == 0x00u);
    CHECK(od_sec2_state() == OD_SEC2_UNCLAIMED && !g_secret[0].present);
    CHECK(od_sec2_boot(NULL) == OD_SEC2_UNCLAIMED);
}

static void test_ident_and_klan(void)
{
    uint8_t a[16], b[16];

    CASE("ident flags and K_lan / K_v1");
    setup(false);
    CHECK(od_sec2_ident_flags() == OD_IDENT_FLAG_SEC_V2);
    CHECK(!od_sec2_k_lan(a) && !od_sec2_k_v1(a));
    setup(true);
    CHECK(od_sec2_ident_flags() == (OD_IDENT_FLAG_SEC_V2 | OD_IDENT_FLAG_CLAIMED));
    CHECK(od_sec2_k_v1(a) && memcmp(a, MASTER, 16u) == 0);   /* migrated: K_v1 is the 2.x key */
    CHECK(od_sec2_k_lan(a));
    sec2c_kdf(MASTER, "OD2/k-lan", UID, 8u, b);
    CHECK(memcmp(a, b, 16u) == 0);
}

#ifndef SEC2_CTR_SUITE
int main(void)
{
    test_primitives();
    test_point_validation();
    test_records();
    test_boot();
    test_auth_happy();
    test_auth_failures();
    test_rate_limit();
    test_fragments();
    test_records_layer();
    test_plaintext_set();
    test_claim();
    test_sec_key();
    test_ident_and_klan();
    printf("sec2: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
#endif /* SEC2_CTR_SUITE */
