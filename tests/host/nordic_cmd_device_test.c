/* nordic_cmd_device_test.c -- targets/nordic-zephyr/src/od_cmd_device.c, the PRODUCTION file,
 * against fake BLE/LED/buzzer/kernel seams.
 *
 * CAPABILITY BEHAVIOUR IS NOT ROUTING, and dispatch_route_test.c cannot see it: that suite
 * replaces every hook with a recorder, so it proves an opcode reaches its own hook and nothing
 * about what the hook then does. The two answers this repo made deliberate decisions about are
 * exactly the ones that would slip through:
 *
 *   0x52  a wire change. This target has no power latch and used to fall silent; it now answers
 *         the canonical unsupported NACK. Wrong bytes, a sealed frame instead of a plaintext one,
 *         or an OK verdict would all be invisible to every other suite and to the build.
 *   0x53  recognised and DELIBERATELY silent, matching the reference nRF52840 build. The failure
 *         mode is the opposite one -- a reply appearing where none should.
 *
 * 0x43 is here for a third reason: ESP32 answered nothing to it from C8 until C11.2, because a
 * handler existed and nothing routed to it. Nothing tested the reply shape on either target.
 */

#include "od_cmd_app.h"
#include "od_cmd_test_ctx.h"

#include "od_cmd_reply.h"
#include "opendisplay_ble.h"
#include "opendisplay_buzzer.h"
#include "opendisplay_led.h"
#include "opendisplay_protocol.h"
#include "zephyr/kernel.h"

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

/* ------------------------------------------------------------------------------ fake reply --- */

#define SENT_MAX 8u
static struct { uint16_t len; bool plain; uint8_t data[64]; } g_sent[SENT_MAX];
static unsigned g_sent_n;

static void record(const uint8_t *frame, uint16_t len, bool plain)
{
    if (g_sent_n < SENT_MAX) {
        g_sent[g_sent_n].len = len;
        g_sent[g_sent_n].plain = plain;
        memcpy(g_sent[g_sent_n].data, frame,
               (len < sizeof g_sent[0].data) ? len : sizeof g_sent[0].data);
        ++g_sent_n;
    }
}

od_txq_status_t od_cmd_reply(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    (void)ctx;
    record(frame, len, false);
    return OD_TXQ_OK;
}

od_txq_status_t od_cmd_reply_plain(const od_cmd_ctx_t *ctx, const uint8_t *frame, uint16_t len)
{
    (void)ctx;
    record(frame, len, true);
    return OD_TXQ_OK;
}

void od_cmd_flush_before_refresh(void) { }

/* ------------------------------------------------------------------------ fake peripherals --- */

static uint16_t g_app_version = 0x0102u;
static uint8_t  g_app_patch = 3u;
static unsigned g_dfu_scheduled;
static unsigned g_deep_sleep_scheduled;
static int      g_led_activate_rc;
static int      g_led_stop_rc;
static int      g_buzzer_rc;
static unsigned g_led_activates;
static unsigned g_led_stops;
static bool     g_led_stop_had_index;

uint16_t opendisplay_ble_get_app_version(void)       { return g_app_version; }
uint8_t  opendisplay_ble_get_app_version_patch(void) { return g_app_patch; }
void     opendisplay_ble_schedule_dfu(void)          { ++g_dfu_scheduled; }
void     opendisplay_ble_schedule_deep_sleep(void)   { ++g_deep_sleep_scheduled; }

void opendisplay_ble_copy_msd_bytes(uint8_t out[16])
{
    unsigned i;
    for (i = 0u; i < 16u; ++i) {
        out[i] = (uint8_t)(0xB0u + i);
    }
}

int opendisplay_led_activate(uint8_t index, const uint8_t *payload, uint16_t len)
{
    (void)index; (void)payload; (void)len;
    ++g_led_activates;
    return g_led_activate_rc;
}

int opendisplay_led_stop(uint8_t index, bool has_index)
{
    (void)index;
    ++g_led_stops;
    g_led_stop_had_index = has_index;
    return g_led_stop_rc;
}

int opendisplay_buzzer_activate(const uint8_t *payload, uint16_t len)
{
    (void)payload; (void)len;
    return g_buzzer_rc;
}

uint32_t fake_k_sleep_ms;
uint32_t fake_k_uptime_ms;
unsigned fake_nvic_resets;
void fake_zephyr_reset(void)   { fake_k_sleep_ms = 0u; }
void k_msleep(int32_t ms)      { fake_k_sleep_ms += (uint32_t)((ms > 0) ? ms : 0); }
uint32_t k_uptime_get_32(void) { return fake_k_uptime_ms; }
void NVIC_SystemReset(void)    { ++fake_nvic_resets; }

void _od_log(int level, const char *fmt, ...);
void _od_log(int level, const char *fmt, ...)
{
    va_list ap;
    (void)level; (void)fmt;
    va_start(ap, fmt);
    va_end(ap);
}

/* --------------------------------------------------------------------------------- helpers --- */

static od_cmd_ctx_t CTX;

static void reset_all(void)
{
    CTX = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u }, NULL, 2u, false);
    memset(g_sent, 0, sizeof g_sent);
    g_sent_n = 0u;
    g_dfu_scheduled = 0u;
    g_deep_sleep_scheduled = 0u;
    g_led_activate_rc = 0;
    g_led_stop_rc = 0;
    g_buzzer_rc = 0;
    g_led_activates = 0u;
    g_led_stops = 0u;
    g_led_stop_had_index = false;
    fake_nvic_resets = 0u;
}

static od_span_t body(const uint8_t *p, uint16_t n) { return od_span_make(p, n); }
static od_span_t empty(void) { return od_span_make(NULL, 0u); }

/* ----------------------------------------------------------------------------------- cases --- */

/* THE ONE DELIBERATE WIRE CHANGE IN C11. Every byte of it is pinned, because "unsupported" is a
 * thing a host acts on and there is no other coverage anywhere. */
static void test_power_off_is_the_canonical_unsupported_nack(void)
{
    CASE("0x52 answers {FF,52,OD_ERR_POWER_OFF_UNSUPPORTED,00}");
    reset_all();
    CHECK(od_cmd_app_power_off(&CTX, empty()) == OD_CMD_NACK);
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == 4u);
    CHECK(g_sent[0].data[0] == RESP_NACK);
    CHECK(g_sent[0].data[1] == RESP_POWER_OFF);
    CHECK(g_sent[0].data[2] == OD_ERR_POWER_OFF_UNSUPPORTED);
    CHECK(g_sent[0].data[3] == 0x00u);

    CASE("and it is PLAINTEXT -- a hard NACK is never sealed");
    CHECK(g_sent[0].plain);

    CASE("NACK, not UNKNOWN: the frame was recognised");
    /* Both refuse to stamp activity, so this is not about the clock -- it is about the wire. An
     * UNKNOWN verdict would mean the shared map answered nothing, and 0x52 answers. */
    reset_all();
    CHECK(od_cmd_app_power_off(&CTX, empty()) != OD_CMD_UNKNOWN);

    CASE("a payload does not change the answer");
    reset_all();
    {
        const uint8_t junk[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
        CHECK(od_cmd_app_power_off(&CTX, body(junk, sizeof junk)) == OD_CMD_NACK);
        CHECK(g_sent_n == 1u);
        CHECK(g_sent[0].data[1] == RESP_POWER_OFF);
    }
}

/* THE OPPOSITE FAILURE MODE. Deep sleep is acted on and answered with nothing, matching the
 * reference nRF52840 build, so clients do not treat it as supported here. A reply appearing would
 * be the regression. */
static void test_deep_sleep_is_recognised_and_silent(void)
{
    CASE("0x53 schedules deep sleep and sends NOTHING");
    reset_all();
    CHECK(od_cmd_app_deep_sleep(&CTX, empty()) == OD_CMD_OK);
    CHECK(g_deep_sleep_scheduled == 1u);
    CHECK(g_sent_n == 0u);
}

static void test_firmware_version_shape(void)
{
    CASE("0x43 answers [00][43][major][minor][shaLen][sha...][patch]");
    reset_all();
    g_app_version = 0x0102u;
    g_app_patch = 3u;
    CHECK(od_cmd_app_firmware_version(&CTX, empty()) == OD_CMD_OK);
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].data[0] == 0x00u);
    CHECK(g_sent[0].data[1] == RESP_FIRMWARE_VERSION);
    CHECK(g_sent[0].data[2] == 0x01u);            /* major */
    CHECK(g_sent[0].data[3] == 0x02u);            /* minor */
    {
        const uint8_t sha_len = g_sent[0].data[4];
        CHECK(sha_len <= 40u);
        /* The patch byte TRAILS the sha, so a host that stops after it keeps working. Its position
         * is therefore a function of sha_len, and that relationship is the contract. */
        CHECK(g_sent[0].len == (uint16_t)(5u + sha_len + 1u));
        CHECK(g_sent[0].data[5u + sha_len] == 3u);
    }

    CASE("and it is PLAINTEXT -- a client must identify a device before it can authenticate");
    CHECK(g_sent[0].plain);
}

static void test_read_msd_shape(void)
{
    CASE("0x44 answers [00][44] plus the 16 MSD bytes, sealed");
    reset_all();
    CHECK(od_cmd_app_read_msd(&CTX, empty()) == OD_CMD_OK);
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == 18u);
    CHECK(g_sent[0].data[0] == 0x00u);
    CHECK(g_sent[0].data[1] == RESP_MSD_READ);
    CHECK(g_sent[0].data[2] == 0xB0u);
    CHECK(g_sent[0].data[17] == 0xBFu);
    CHECK(!g_sent[0].plain);                      /* an application response, not a control frame */
}

static void test_dfu_acks_before_scheduling(void)
{
    CASE("0x51 acks and then schedules -- the ack must not depend on surviving the switch");
    reset_all();
    CHECK(od_cmd_app_enter_dfu(&CTX, empty()) == OD_CMD_OK);
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].len == 2u);
    CHECK(g_sent[0].data[0] == 0x00u);
    CHECK(g_sent[0].data[1] == RESP_ENTER_DFU);
    CHECK(g_dfu_scheduled == 1u);
}

static void test_reboot_resets_and_says_nothing(void)
{
    CASE("0x0F resets; the reply is the absence of one");
    reset_all();
    (void)od_cmd_app_reboot(&CTX, empty());
    CHECK(fake_nvic_resets == 1u);
    CHECK(g_sent_n == 0u);
}

static void test_led_and_buzzer_verdicts(void)
{
    const uint8_t one[1] = { 0x02u };

    CASE("LED activate with an empty body is refused with error 0x01, plaintext");
    reset_all();
    CHECK(od_cmd_app_led_activate(&CTX, empty()) == OD_CMD_NACK);
    CHECK(g_led_activates == 0u);                 /* refused before the driver is touched */
    CHECK(g_sent_n == 1u);
    CHECK(g_sent[0].data[0] == 0xFFu);
    CHECK(g_sent[0].data[2] == 0x01u);
    CHECK(g_sent[0].plain);

    CASE("a driver refusal is error 0x02, also plaintext");
    reset_all();
    g_led_activate_rc = -1;
    CHECK(od_cmd_app_led_activate(&CTX, body(one, sizeof one)) == OD_CMD_NACK);
    CHECK(g_sent[0].data[2] == 0x02u);
    CHECK(g_sent[0].plain);

    CASE("success is a sealed 4-byte ack");
    reset_all();
    CHECK(od_cmd_app_led_activate(&CTX, body(one, sizeof one)) == OD_CMD_OK);
    CHECK(g_sent[0].len == 4u);
    CHECK(g_sent[0].data[0] == 0x00u);
    CHECK(g_sent[0].data[1] == RESP_LED_ACTIVATE_ACK);
    CHECK(!g_sent[0].plain);

    /* An empty LED_STOP body means "all", a non-empty one names an index. Two different commands
     * behind one opcode, and the boundary is the body length. */
    CASE("LED stop distinguishes 'this one' from 'all' by whether a body is present");
    reset_all();
    CHECK(od_cmd_app_led_stop(&CTX, body(one, sizeof one)) == OD_CMD_OK);
    CHECK(g_led_stop_had_index);
    reset_all();
    CHECK(od_cmd_app_led_stop(&CTX, empty()) == OD_CMD_OK);
    CHECK(!g_led_stop_had_index);

    CASE("the buzzer's driver code is carried into the error frame");
    reset_all();
    g_buzzer_rc = 7;
    CHECK(od_cmd_app_buzzer(&CTX, empty()) == OD_CMD_NACK);
    CHECK(g_sent[0].data[0] == 0xFFu);
    CHECK(g_sent[0].data[1] == RESP_BUZZER_ACK);
    CHECK(g_sent[0].data[2] == 7u);
    CHECK(g_sent[0].plain);
}

/* Claim mode is never active here: the planned reboot always proceeds. */
bool od_claim_mode_before_reboot(void) { return true; }

int main(void)
{
    test_power_off_is_the_canonical_unsupported_nack();
    test_deep_sleep_is_recognised_and_silent();
    test_firmware_version_shape();
    test_read_msd_shape();
    test_dfu_acks_before_scheduling();
    test_reboot_resets_and_says_nothing();
    test_led_and_buzzer_verdicts();

    printf("nordic_cmd_device: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
