/* gate_test.c -- one case per od_gate_admit() outcome, checking the wire bytes AND the outcome.
 *
 * The compiler already guarantees the od_sec2_open switch is exhaustive (od_gate.h explains the
 * -Wswitch ratchet). What it cannot check is that each row of the gating table maps to the RIGHT
 * wire action, which is where the security consequences live: FE goes out plaintext because the
 * peer has no session to read it with, the record-layer NACKs F1-F3 go out sealed, the third bad
 * tag ends the session after its NACK, and a no-ack opcode draws silence because a NACK would kill
 * the client's upload loop.
 *
 * The session is a real one: SEC_AUTH through tests/host/sec2_client.c (NOT independent of
 * od_sec2, see its header), and every H->D record is sealed by that client.
 */

#include "od_gate.h"

#include "od_reply.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
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

#define SENT_MAX 16u
static struct { uint16_t len; uint8_t data[OD_TX_FRAME_MAX]; } g_sent[SENT_MAX];
static unsigned g_sent_n;
static unsigned g_pop;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag;
    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].len = len;
        memcpy(g_sent[g_sent_n].data, frame, len);
        ++g_sent_n;
    }
    return OD_RADIO_SENT;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    (void)origin; (void)tag;
    return true;
}

bool sec2c_radio_pop(uint8_t *buf, uint16_t *len)
{
    if (g_pop >= g_sent_n) {
        return false;
    }
    memcpy(buf, g_sent[g_pop].data, g_sent[g_pop].len);
    *len = g_sent[g_pop].len;
    ++g_pop;
    return true;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

/* ------------------------------------------------------------------------- od_sec2_app hooks --- */

/* Claim mode is set per case; the other three claim-mode hooks are here only because defining
 * one of them means od_sec2_hooks.c's forwarders must not be linked. */
static struct od_sec2_claim_mode g_cm;

uint32_t od_sec2_app_now_ms(void) { return 1000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return &g_sec; }
void od_sec2_app_claim_mode(struct od_sec2_claim_mode *out) { *out = g_cm; }
void od_sec2_app_claim_event(enum od_sec2_claim_event ev, uint32_t arg) { (void)ev; (void)arg; }
bool od_sec2_app_scrub(void) { return false; }
void od_sec2_app_rest_sync(void) { }

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE  = { OD_ORIGIN_BLE, 3u };
static const od_reply_t BLE2 = { OD_ORIGIN_BLE, 4u };
static const od_reply_t LAN  = { OD_ORIGIN_LAN_PLAIN, 5u };
static const od_reply_t TLS  = { OD_ORIGIN_LAN_TLS, 6u };

static uint8_t g_scratch[OD_SEC2_PLAIN_MAX];
static struct sec2c C;
static const uint8_t PAYLOAD[4] = { 1u, 2u, 3u, 4u };

static void clear_sent(void)
{
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_pop = 0u;
}

enum dev { UNCLAIMED, CLAIMED, LIVE };

static void setup(enum dev d)
{
    fake_reset();
    sec_init(0);
    od_txq_reset();
    memset(&g_cm, 0, sizeof g_cm);
    clear_sent();
    (void)od_sec2_boot(d == UNCLAIMED ? NULL : &g_sec);
    sec2c_init(&C, MASTER, UID);
    if (d == LIVE) {
        CHECK(sec2c_auth(&C, &BLE) == 0);
    }
    clear_sent();
}

/* A claimed device whose OS2K is corrupt while 0x27 carries claim evidence: boot row 3. */
static void setup_locked(void)
{
    struct SecurityConfig ev;

    setup(CLAIMED);
    g_secret[OD_HAL_SECRET_KEY].data[10] ^= 1u;
    memset(&ev, 0, sizeof ev);
    ev.reserved[0] = OD_SEC27_AT_REST_SEC2;
    CHECK(od_sec2_boot(&ev) == OD_SEC2_LOCKED);
    clear_sent();
}

/* Run the gate once on `body` with a fresh one-unit reservation, then drain the queue. */
static od_gate_result_t admit(const od_reply_t *rp, uint16_t cmd, const uint8_t *body,
                              uint16_t n, uint8_t *scratch, size_t cap)
{
    od_tx_reservation_t r;
    od_gate_result_t g;

    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    g = od_gate_admit(&r, rp, cmd, od_span_make(body, n), scratch, cap);
    od_txq_release(&r);
    (void)od_txq_process();
    return g;
}

/* Seal PAYLOAD as `cmd` and admit it; the record (after the opcode) is left in rec/rec_n. */
static uint8_t g_rec[64];
static uint16_t g_rec_n;
static od_gate_result_t admit_sealed(uint16_t cmd)
{
    uint8_t f[64];
    const uint16_t n = sec2c_seal(&C, cmd, PAYLOAD, sizeof PAYLOAD, f);

    g_rec_n = (uint16_t)(n - 2u);
    memcpy(g_rec, f + 2, g_rec_n);
    return admit(&BLE, cmd, g_rec, g_rec_n, g_scratch, sizeof g_scratch);
}

static bool sent_plain(unsigned i, uint8_t b0, uint16_t cmd, uint8_t code)
{
    return g_sent[i].len == 3u && g_sent[i].data[0] == b0 &&
           g_sent[i].data[1] == (uint8_t)cmd && g_sent[i].data[2] == code;
}

/* The i-th captured frame is a sealed [FF][cmd_lo][code]. */
static bool sent_sealed_nack(unsigned i, uint16_t cmd, uint8_t code)
{
    uint8_t p[OD_TX_FRAME_MAX];
    uint16_t pn = 0u;

    return g_sent[i].data[0] == OD_SEC2_SEALED_STATUS &&
           sec2c_open(&C, g_sent[i].data, g_sent[i].len, p, &pn) && pn == 3u &&
           p[0] == RESP_NACK && p[1] == (uint8_t)cmd && p[2] == code;
}

/* ------------------------------------------------------------------------------- the table --- */

static void test_locked(void)
{
    od_gate_result_t g;

    CASE("LOCKED: [00][cmd][FE] plaintext on BLE, AUTH_REQUIRED");
    setup_locked();
    g = admit(&BLE, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_ACK, 0x0071u, RESP_AUTH_REQUIRED));

    CASE("LOCKED: FE on TLS too -- every origin");
    clear_sent();
    g = admit(&TLS, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_ACK, 0x0071u, RESP_AUTH_REQUIRED));
}

static void test_unclaimed(void)
{
    od_gate_result_t g;

    CASE("UNCLAIMED, BLE: the body runs as sent, unsealed, nothing queued");
    setup(UNCLAIMED);
    g = admit(&BLE, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_ACCEPTED);
    CHECK(g.body.p == PAYLOAD && g.body.n == sizeof PAYLOAD);
    CHECK(!g.sealed);
    CHECK(g_sent_n == 0u);

    CASE("UNCLAIMED, LAN plain: likewise");
    g = admit(&LAN, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_ACCEPTED && !g.sealed && g_sent_n == 0u);

    CASE("UNCLAIMED in claim mode: a command off the allow-list is [FF][cmd][FD] plaintext");
    g_cm.active = true;
    g = admit(&BLE, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_HANDLER_NACK);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_NACK, 0x0071u, OD_GATE_CLAIM_MODE));

    CASE("UNCLAIMED in claim mode: an allow-listed command runs");
    clear_sent();
    g = admit(&BLE, CMD_CONFIG_READ, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_ACCEPTED && g_sent_n == 0u);
}

static void test_claimed_without_a_session(void)
{
    od_gate_result_t g;

    CASE("CLAIMED, LAN TLS: plaintext runs, TLS protects it");
    setup(CLAIMED);
    g = admit(&TLS, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_ACCEPTED);
    CHECK(g.body.p == PAYLOAD && !g.sealed && g_sent_n == 0u);

    CASE("CLAIMED, LAN plain: [00][cmd][FE]");
    g = admit(&LAN, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_ACK, 0x0071u, RESP_AUTH_REQUIRED));

    CASE("CLAIMED, BLE, no session: [00][cmd][FE], and no-ack opcodes are refused the same way");
    clear_sent();
    g = admit(&BLE, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_ACK, 0x0071u, RESP_AUTH_REQUIRED));
    clear_sent();
    g = admit(&BLE, CMD_PIPE_WRITE_DATA, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_ACK, CMD_PIPE_WRITE_DATA, RESP_AUTH_REQUIRED));

    CASE("CLAIMED, BLE, a session that belongs to another connection: FE");
    setup(LIVE);
    g = admit(&BLE2, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_ACK, 0x0071u, RESP_AUTH_REQUIRED));
}

static void test_open_ok(void)
{
    od_gate_result_t g;

    CASE("OPEN_OK: the payload is returned from the scratch, sealed, and nothing is queued");
    setup(LIVE);
    g = admit_sealed(0x0071u);
    CHECK(g.outcome == OD_FRAME_ACCEPTED);
    CHECK(g.sealed);
    CHECK(g.body.p == g_scratch);
    CHECK(g.body.n == sizeof PAYLOAD && memcmp(g.body.p, PAYLOAD, sizeof PAYLOAD) == 0);
    CHECK(g_sent_n == 0u);

    CASE("a live session in claim mode: off the allow-list is a SEALED [FF][cmd][FD]");
    g_cm.active = true;
    g = admit_sealed(0x0071u);
    CHECK(g.outcome == OD_FRAME_HANDLER_NACK);
    CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, OD_GATE_CLAIM_MODE));

    CASE("a live session in claim mode after the claim: a SEALED CONFIG_WRITE runs, even locked");
    setup(LIVE);
    g_cm.active = true;
    g_cm.config_locked = true;
    g = admit_sealed(CMD_CONFIG_WRITE);
    CHECK(g.outcome == OD_FRAME_ACCEPTED && g.sealed);
    CHECK(g_sent_n == 0u);
    g = admit_sealed(CMD_SEC_KEY);
    CHECK(g.outcome == OD_FRAME_HANDLER_NACK);
}

static void test_open_record(void)
{
    od_gate_result_t g;

    CASE("OPEN_RECORD: shorter than ctr+tag is a sealed [FF][cmd][F1], session kept");
    setup(LIVE);
    g = admit(&BLE, 0x0071u, PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, OD_SEC_NACK_RECORD));
    CHECK(od_sec2_session_live(&BLE));
}

static void test_open_replay(void)
{
    od_gate_result_t g;

    CASE("OPEN_REPLAY: the same record twice is a sealed [FF][cmd][F2] the second time");
    setup(LIVE);
    g = admit_sealed(0x0071u);
    CHECK(g.outcome == OD_FRAME_ACCEPTED);
    g = admit(&BLE, 0x0071u, g_rec, g_rec_n, g_scratch, sizeof g_scratch);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, OD_SEC_NACK_REPLAY));
    CHECK(od_sec2_session_live(&BLE));
}

static void test_open_bad_tag(void)
{
    od_gate_result_t g;
    unsigned i;

    CASE("OPEN_BAD_TAG: a flipped tag bit is a sealed [FF][cmd][F3], body never returned");
    setup(LIVE);
    for (i = 0u; i < 2u; ++i) {
        uint8_t f[64];
        const uint16_t n = sec2c_seal(&C, 0x0071u, PAYLOAD, sizeof PAYLOAD, f);
        f[n - 1u] ^= 0x01u;
        clear_sent();
        g = admit(&BLE, 0x0071u, f + 2, (uint16_t)(n - 2u), g_scratch, sizeof g_scratch);
        CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
        CHECK(g.body.n == 0u);
        CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, OD_SEC_NACK_INTEGRITY));
        CHECK(od_sec2_session_live(&BLE));
    }

    CASE("OPEN_BAD_TAG_TEARDOWN: the third strike's F3 still goes out sealed, then the session ends");
    {
        uint8_t f[64];
        const uint16_t n = sec2c_seal(&C, 0x0071u, PAYLOAD, sizeof PAYLOAD, f);
        f[n - 1u] ^= 0x01u;
        clear_sent();
        g = admit(&BLE, 0x0071u, f + 2, (uint16_t)(n - 2u), g_scratch, sizeof g_scratch);
    }
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, OD_SEC_NACK_INTEGRITY));
    CHECK(!od_sec2_session_live(&BLE));

    CASE("and the next record meets FE, plaintext");
    clear_sent();
    g = admit_sealed(0x0071u);
    CHECK(g.outcome == OD_FRAME_AUTH_REQUIRED);
    CHECK(g_sent_n == 1u && sent_plain(0u, RESP_ACK, 0x0071u, RESP_AUTH_REQUIRED));
}

static void test_open_crypto_error_and_no_room(void)
{
    od_gate_result_t g;

    CASE("OPEN_CRYPTO_ERROR: an engine fault is a sealed [FF][cmd][00], no strike");
    setup(LIVE);
    g_force_status = OD_HAL_CRYPTO_ERROR;
    g_force_after = (int32_t)g_hal_calls;
    g_force_once = true;
    g = admit_sealed(0x0071u);
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, 0x00u));
    CHECK(od_sec2_session_live(&BLE));

    CASE("OPEN_NO_ROOM: a scratch too small for the payload is a sealed [FF][cmd][00]");
    setup(LIVE);
    {
        uint8_t f[64];
        uint8_t tiny[2];
        const uint16_t n = sec2c_seal(&C, 0x0071u, PAYLOAD, sizeof PAYLOAD, f);
        g = admit(&BLE, 0x0071u, f + 2, (uint16_t)(n - 2u), tiny, sizeof tiny);
    }
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, 0x00u));

    CASE("a NULL scratch likewise");
    clear_sent();
    {
        uint8_t f[64];
        const uint16_t n = sec2c_seal(&C, 0x0071u, PAYLOAD, sizeof PAYLOAD, f);
        g = admit(&BLE, 0x0071u, f + 2, (uint16_t)(n - 2u), NULL, 0u);
    }
    CHECK(g.outcome == OD_FRAME_CRYPTO_FAILED);
    CHECK(g_sent_n == 1u && sent_sealed_nack(0u, 0x0071u, 0x00u));
}

static void test_no_ack_opcodes_are_silent(void)
{
    static const uint16_t quiet[2] = { CMD_PIPE_WRITE_DATA, CMD_DEEP_SLEEP };
    od_gate_result_t g;
    unsigned i;

    for (i = 0u; i < 2u; ++i) {
        CASE("no-ack opcode: a replay is CRYPTO_DROPPED with nothing on the wire");
        setup(LIVE);
        g = admit_sealed(quiet[i]);
        CHECK(g.outcome == OD_FRAME_ACCEPTED);
        g = admit(&BLE, quiet[i], g_rec, g_rec_n, g_scratch, sizeof g_scratch);
        CHECK(g.outcome == OD_FRAME_CRYPTO_DROPPED);
        CHECK(g_sent_n == 0u);

        CASE("no-ack opcode: a short record is silent too");
        g = admit(&BLE, quiet[i], PAYLOAD, sizeof PAYLOAD, g_scratch, sizeof g_scratch);
        CHECK(g.outcome == OD_FRAME_CRYPTO_DROPPED);
        CHECK(g_sent_n == 0u);

        CASE("no-ack opcode: the third bad tag is silent and still ends the session");
        {
            unsigned k;
            for (k = 0u; k < OD_SEC2_STRIKES; ++k) {
                uint8_t f[64];
                const uint16_t n = sec2c_seal(&C, quiet[i], PAYLOAD, sizeof PAYLOAD, f);
                f[n - 1u] ^= 0x01u;
                g = admit(&BLE, quiet[i], f + 2, (uint16_t)(n - 2u), g_scratch,
                          sizeof g_scratch);
                CHECK(g.outcome == OD_FRAME_CRYPTO_DROPPED);
            }
        }
        CHECK(g_sent_n == 0u);
        CHECK(!od_sec2_session_live(&BLE));
    }

    CASE("the no-ack set is exactly PIPE DATA and DEEP_SLEEP");
    CHECK(od_gate_no_ack(CMD_PIPE_WRITE_DATA));
    CHECK(od_gate_no_ack(CMD_DEEP_SLEEP));
    CHECK(!od_gate_no_ack(CMD_DIRECT_WRITE_DATA));
    CHECK(!od_gate_no_ack(CMD_PIPE_WRITE_END));
}

static void test_claim_mode_allow_list(void)
{
    CASE("claim mode always admits the discovery and handshake set");
    CHECK(od_gate_claim_mode_allows(CMD_REBOOT, false, true, false));
    CHECK(od_gate_claim_mode_allows(CMD_CONFIG_READ, false, true, false));
    CHECK(od_gate_claim_mode_allows(CMD_SEC_STATUS, false, true, false));
    CHECK(od_gate_claim_mode_allows(CMD_SEC_CLAIM, false, true, false));

    CASE("config writes only in a FRESH claim mode before a code is rendered");
    CHECK(od_gate_claim_mode_allows(CMD_CONFIG_WRITE, true, false, false));
    CHECK(od_gate_claim_mode_allows(CMD_CONFIG_CHUNK, true, false, false));
    CHECK(od_gate_claim_mode_allows(CMD_CONFIG_CLEAR, true, false, false));
    CHECK(!od_gate_claim_mode_allows(CMD_CONFIG_WRITE, true, true, false));
    CHECK(!od_gate_claim_mode_allows(CMD_CONFIG_WRITE, false, false, false));

    CASE("everything else is refused");
    CHECK(!od_gate_claim_mode_allows(CMD_DIRECT_WRITE_START, true, false, false));
    CHECK(!od_gate_claim_mode_allows(CMD_SEC_KEY, true, false, false));
    CHECK(!od_gate_claim_mode_allows(CMD_PIPE_WRITE_DATA, true, false, false));

    CASE("after the claim, in the claiming session: config writes, in any kind (7.3, 13.1 step 9)");
    CHECK(od_gate_claim_mode_allows(CMD_CONFIG_WRITE, false, true, true));
    CHECK(od_gate_claim_mode_allows(CMD_CONFIG_CHUNK, false, true, true));
    CHECK(od_gate_claim_mode_allows(CMD_CONFIG_CLEAR, true, true, true));
    CHECK(od_gate_claim_mode_allows(CMD_REBOOT, false, true, true));
    CASE("after the claim, the other CM refusals still apply");
    CHECK(!od_gate_claim_mode_allows(CMD_SEC_KEY, false, true, true));
    CHECK(!od_gate_claim_mode_allows(CMD_DIRECT_WRITE_START, false, true, true));
    CHECK(!od_gate_claim_mode_allows(CMD_ENTER_DFU, false, true, true));
    CHECK(!od_gate_claim_mode_allows(CMD_LED_ACTIVATE, false, true, true));
}

int main(void)
{
    test_locked();
    test_unclaimed();
    test_claimed_without_a_session();
    test_open_ok();
    test_open_record();
    test_open_replay();
    test_open_bad_tag();
    test_open_crypto_error_and_no_room();
    test_no_ack_opcodes_are_silent();
    test_claim_mode_allow_list();

    printf("gate: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
