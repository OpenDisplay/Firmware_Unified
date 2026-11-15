/* dispatch_route_test.c -- the opcode-to-hook map in od_dispatch.c, one case per canonical opcode.
 *
 * WHY THIS EXISTS AS ITS OWN SUITE. The map used to be a switch inside each target, which meant a
 * new opcode could be routed on one target and unknown on the other, and nothing said so. Moving
 * it into shared/ only helps if the map itself is pinned: this test invokes every canonical opcode
 * through od_dispatch_frame() and records WHICH hook ran. Routing an opcode to its neighbour is
 * then a failure rather than a subtly wrong device.
 *
 * "Unknown" is wire-visible, which is why the unknown case is here too: od_frame_policy gives
 * OD_FRAME_UNKNOWN_OPCODE no activity stamp, so an unrecognised opcode cannot hold an exclusive
 * link open. An opcode that quietly becomes recognised changes that.
 *
 * A NEW OPCODE MUST FAIL THIS TEST until its route and its capability behaviour are chosen.
 */

#include "od_dispatch.h"

#include "od_cmd_app.h"
#include "od_config_read.h"
#include "od_reply.h"
#include "od_pipe.h"
#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_xfer.h"
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

static unsigned g_sent_n;

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag; (void)frame; (void)len;
    ++g_sent_n;
    return OD_RADIO_SENT;
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

/* ------------------------------------------------------------------------- od_sec2_app hooks --- */

static uint32_t g_now_ms = 1000u;

uint32_t od_sec2_app_now_ms(void) { return g_now_ms; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return OD_SEC2_DFLAG_HAS_PANEL; }
const struct SecurityConfig *od_sec2_app_security(void) { return NULL; }

bool od_cmd_mutates_config(uint16_t cmd) { (void)cmd; return false; }

/* --------------------------------------------------------------------------- the fake hooks --- */

/* One slot per hook. The identity recorded is the HOOK, not the opcode: an opcode that reached the
 * wrong hook is exactly the defect this suite exists to catch, and comparing the opcode a hook was
 * handed against the opcode sent would agree with itself no matter how the switch was wired. */
enum hook_id {
    HOOK_NONE = 0,
    HOOK_REBOOT, HOOK_FIRMWARE_VERSION, HOOK_READ_MSD, HOOK_ENTER_DFU, HOOK_POWER_OFF,
    HOOK_DEEP_SLEEP, HOOK_CONFIG_READ, HOOK_CONFIG_WRITE, HOOK_CONFIG_CHUNK, HOOK_CONFIG_CLEAR,
    HOOK_DIRECT_START, HOOK_DIRECT_DATA, HOOK_DIRECT_END, HOOK_PARTIAL_START,
    HOOK_PIPE_START, HOOK_PIPE_DATA, HOOK_PIPE_END,
    HOOK_LED_ACTIVATE, HOOK_LED_STOP, HOOK_BUZZER, HOOK_NFC
};

static enum hook_id g_ran;
static unsigned     g_hook_calls;
static uint16_t     g_body_len;
/* The reply context the hook was handed. A handler decides real things from it -- which transport
 * owns an in-flight transfer, whether a config write may proceed on an authenticated TLS link --
 * so dispatch passing through anything other than what the ingress built is a defect. */
static od_reply_t   g_hook_rp;

/* Result the next hook returns. Set per case so the outcome mapping can be checked alongside the
 * route -- a hook that runs but whose verdict is dropped is as wrong as one that never runs. */
static od_cmd_result_t g_hook_result;

static od_cmd_result_t record(enum hook_id id, const od_cmd_ctx_t *ctx, od_span_t body)
{
    g_ran = id;
    ++g_hook_calls;
    g_body_len = (uint16_t)body.n;
    g_hook_rp = ctx->rp;
    return g_hook_result;
}

#define HOOK(fn, id)                                                     \
    od_cmd_result_t fn(const od_cmd_ctx_t *ctx, od_span_t body)          \
    { return record(id, ctx, body); }

HOOK(od_cmd_app_reboot,           HOOK_REBOOT)
HOOK(od_cmd_app_firmware_version, HOOK_FIRMWARE_VERSION)
HOOK(od_cmd_app_read_msd,         HOOK_READ_MSD)
HOOK(od_cmd_app_enter_dfu,        HOOK_ENTER_DFU)
HOOK(od_cmd_app_power_off,        HOOK_POWER_OFF)
HOOK(od_cmd_app_deep_sleep,       HOOK_DEEP_SLEEP)
HOOK(od_cmd_app_config_read,      HOOK_CONFIG_READ)
HOOK(od_cmd_app_config_write,     HOOK_CONFIG_WRITE)
HOOK(od_cmd_app_config_chunk,     HOOK_CONFIG_CHUNK)
HOOK(od_cmd_app_config_clear,     HOOK_CONFIG_CLEAR)
HOOK(od_xfer_direct_start,        HOOK_DIRECT_START)
HOOK(od_xfer_data,                HOOK_DIRECT_DATA)
HOOK(od_xfer_end,                 HOOK_DIRECT_END)
HOOK(od_xfer_partial_start,       HOOK_PARTIAL_START)
HOOK(od_pipe_start,               HOOK_PIPE_START)
HOOK(od_pipe_data,                HOOK_PIPE_DATA)
HOOK(od_pipe_end,                 HOOK_PIPE_END)
HOOK(od_cmd_app_led_activate,     HOOK_LED_ACTIVATE)
HOOK(od_cmd_app_led_stop,         HOOK_LED_STOP)
HOOK(od_cmd_app_buzzer,           HOOK_BUZZER)
HOOK(od_nfc_frame,                HOOK_NFC)

/* --------------------------------------------------------------------------------- helpers --- */

static const od_reply_t BLE = { OD_ORIGIN_BLE, 9u };

static void setup(void)
{
    fake_reset();
    sec_init(0);
    (void)od_sec2_boot(NULL);
    od_config_read_cancel();
    od_txq_reset();
    g_sent_n = 0u;
    g_ran = HOOK_NONE;
    g_hook_calls = 0u;
    g_body_len = 0u;
    g_hook_result = OD_CMD_OK;
    memset(&g_hook_rp, 0, sizeof g_hook_rp);
}

/* The device is UNCLAIMED for the whole suite (od_sec2_boot(NULL) over an empty secret store), so
 * every frame is plaintext and reaches its hook without a session. Routing is what is under test here; the gate
 * has its own suite. */
static od_frame_outcome_t send_cmd(uint16_t cmd, uint16_t body_len)
{
    uint8_t frame[16];
    unsigned i;

    frame[0] = (uint8_t)((cmd >> 8) & 0xFFu);
    frame[1] = (uint8_t)(cmd & 0xFFu);
    for (i = 0; i < body_len && (2u + i) < sizeof frame; ++i) {
        frame[2u + i] = (uint8_t)(0xA0u + i);
    }
    return od_dispatch_frame(&BLE, od_span_make(frame, (uint16_t)(2u + body_len)));
}

static void route(uint16_t cmd, enum hook_id want, od_frame_outcome_t want_outcome)
{
    od_frame_outcome_t got;

    setup();
    got = send_cmd(cmd, 3u);
    CHECK(g_hook_calls == 1u);
    CHECK(g_ran == want);
    CHECK(g_body_len == 3u);
    CHECK(got == want_outcome);
}

/* ----------------------------------------------------------------------------------- cases --- */

static void test_every_opcode_reaches_its_own_hook(void)
{
    CASE("every canonical opcode routes to its own hook");

    route(CMD_REBOOT,              HOOK_REBOOT,           OD_FRAME_ACCEPTED);
    route(CMD_CONFIG_READ,         HOOK_CONFIG_READ,      OD_FRAME_ACCEPTED);
    route(CMD_CONFIG_WRITE,        HOOK_CONFIG_WRITE,     OD_FRAME_ACCEPTED);
    route(CMD_CONFIG_CHUNK,        HOOK_CONFIG_CHUNK,     OD_FRAME_ACCEPTED);
    route(CMD_CONFIG_CLEAR,        HOOK_CONFIG_CLEAR,     OD_FRAME_ACCEPTED);
    route(CMD_ENTER_DFU,           HOOK_ENTER_DFU,        OD_FRAME_ACCEPTED);
    route(CMD_POWER_OFF,           HOOK_POWER_OFF,        OD_FRAME_ACCEPTED);
    route(CMD_DEEP_SLEEP,          HOOK_DEEP_SLEEP,       OD_FRAME_ACCEPTED);
    route(CMD_DIRECT_WRITE_START,  HOOK_DIRECT_START,     OD_FRAME_ACCEPTED);
    route(CMD_DIRECT_WRITE_DATA,   HOOK_DIRECT_DATA,      OD_FRAME_ACCEPTED);
    route(CMD_DIRECT_WRITE_END,    HOOK_DIRECT_END,       OD_FRAME_ACCEPTED);
    route(CMD_LED_ACTIVATE,        HOOK_LED_ACTIVATE,     OD_FRAME_ACCEPTED);
    route(CMD_LED_STOP,            HOOK_LED_STOP,         OD_FRAME_ACCEPTED);
    route(CMD_PARTIAL_WRITE_START, HOOK_PARTIAL_START,    OD_FRAME_ACCEPTED);
    route(CMD_BUZZER,              HOOK_BUZZER,           OD_FRAME_ACCEPTED);
    route(CMD_PIPE_WRITE_START,    HOOK_PIPE_START,       OD_FRAME_ACCEPTED);
    route(CMD_PIPE_WRITE_DATA,     HOOK_PIPE_DATA,        OD_FRAME_ACCEPTED);
    route(CMD_PIPE_WRITE_END,      HOOK_PIPE_END,         OD_FRAME_ACCEPTED);
    route(CMD_NFC_ENDPOINT,        HOOK_NFC,              OD_FRAME_ACCEPTED);

    /* FIRMWARE_VERSION is routed BEFORE the session gate and answered DISCOVERY rather than
     * ACCEPTED: a version poll must not stamp activity, or it holds the exclusive link forever. */
    CASE("FIRMWARE_VERSION and READ_MSD reach their hooks pre-gate and are DISCOVERY");
    route(CMD_FIRMWARE_VERSION,    HOOK_FIRMWARE_VERSION, OD_FRAME_DISCOVERY);
    route(CMD_READ_MSD,            HOOK_READ_MSD,         OD_FRAME_DISCOVERY);
}

static void test_authenticate_never_reaches_a_hook(void)
{
    static const uint16_t sec[5] = {
        CMD_AUTHENTICATE, CMD_SEC_STATUS, CMD_SEC_CLAIM, CMD_SEC_AUTH, CMD_SEC_KEY
    };
    unsigned i;

    CASE("AUTHENTICATE and the SEC opcodes are od_sec2's alone -- no target hook sees them");
    for (i = 0u; i < 5u; ++i) {
        setup();
        (void)send_cmd(sec[i], 3u);
        CHECK(g_hook_calls == 0u);
    }
}

static void test_unknown_opcode_reaches_nothing(void)
{
    od_frame_outcome_t got;

    CASE("an opcode with no route runs no hook and stays silent");
    setup();
    got = send_cmd(0x0099u, 3u);
    CHECK(g_hook_calls == 0u);
    CHECK(got == OD_FRAME_UNKNOWN_OPCODE);
    CHECK(g_sent_n == 0u);
    /* The property that matters downstream: no activity stamp, so unknown-command traffic cannot
     * keep an exclusive link alive. */
    CHECK(!od_frame_policy(got).stamp_activity);

    CASE("0x0074 -- the gap between LED_ACTIVATE and LED_STOP -- is not routed");
    setup();
    CHECK(send_cmd(0x0074u, 0u) == OD_FRAME_UNKNOWN_OPCODE);
    CHECK(g_hook_calls == 0u);
}

static void test_hook_verdicts_map_to_outcomes(void)
{
    CASE("a hook's verdict is carried, not re-derived");

    setup();
    g_hook_result = OD_CMD_NACK;
    CHECK(send_cmd(CMD_BUZZER, 1u) == OD_FRAME_HANDLER_NACK);

    setup();
    g_hook_result = OD_CMD_AUTH_REJECTED;
    CHECK(send_cmd(CMD_BUZZER, 1u) == OD_FRAME_AUTH_REQUIRED);

    /* A target that has no implementation for an opcode it must still define says so with
     * UNKNOWN, and the frame is indistinguishable from one the shared switch never routed. */
    setup();
    g_hook_result = OD_CMD_UNKNOWN;
    CHECK(send_cmd(CMD_NFC_ENDPOINT, 1u) == OD_FRAME_UNKNOWN_OPCODE);

    /* Same for FIRMWARE_VERSION's pre-gate arm: a refused version poll is a NACK, not DISCOVERY. */
    setup();
    g_hook_result = OD_CMD_NACK;
    CHECK(send_cmd(CMD_FIRMWARE_VERSION, 1u) == OD_FRAME_HANDLER_NACK);
}

static void test_body_excludes_the_opcode(void)
{
    CASE("a hook is handed the body only -- the two opcode bytes are dispatch's");
    setup();
    CHECK(send_cmd(CMD_LED_ACTIVATE, 0u) == OD_FRAME_ACCEPTED);
    CHECK(g_body_len == 0u);

    setup();
    CHECK(send_cmd(CMD_LED_ACTIVATE, 7u) == OD_FRAME_ACCEPTED);
    CHECK(g_body_len == 7u);
}

/* THE FRAME CONTEXT IS AN ARGUMENT, NOT A GLOBAL, and this is what says so from the shared side.
 * ESP32 used to reconstruct it inside the dispatcher from a pair of globals the ingress had set
 * immediately beforehand -- a per-frame fact with a lifetime longer than its frame, readable by
 * any nested or later path after the caller had moved on. Both ingresses now build an od_reply_t
 * and hand it over, and dispatch must deliver exactly that. */
static void test_reply_context_reaches_the_hook_unchanged(void)
{
    static const od_reply_t ORIGINS[] = {
        { OD_ORIGIN_BLE,       0x11223344u },
        { OD_ORIGIN_LAN_PLAIN, 0x00000000u },
        { OD_ORIGIN_LAN_TLS,   0xDEADBEEFu },
    };
    uint8_t frame[4] = { 0x00u, 0x77u, 0xA0u, 0xA1u };
    unsigned i;

    CASE("every origin and tag arrives at the hook exactly as the ingress built it");
    for (i = 0u; i < sizeof ORIGINS / sizeof ORIGINS[0]; ++i) {
        setup();
        CHECK(od_dispatch_frame(&ORIGINS[i], od_span_make(frame, sizeof frame))
              == OD_FRAME_ACCEPTED);
        CHECK(g_ran == HOOK_BUZZER);
        CHECK(g_hook_rp.origin == ORIGINS[i].origin);
        CHECK(g_hook_rp.tag == ORIGINS[i].tag);
    }
}

int main(void)
{
    test_every_opcode_reaches_its_own_hook();
    test_reply_context_reaches_the_hook_unchanged();
    test_authenticate_never_reaches_a_hook();
    test_unknown_opcode_reaches_nothing();
    test_hook_verdicts_map_to_outcomes();
    test_body_excludes_the_opcode();

    printf("dispatch_route: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
