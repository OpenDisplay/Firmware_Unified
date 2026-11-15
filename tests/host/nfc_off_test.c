/* The 0x0083 machine at OD_CAP_NFC=0.
 *
 * The claim is an ABSENCE, so the suite proves it two ways. Behaviourally: every sub-command
 * answers nothing at all, returns OD_CMD_UNKNOWN, and reaches no seam -- which is what
 * py-opendisplay's NfcNotSupportedError detection rests on, and why manufacturing an "unsupported"
 * NACK would be inventing a wire meaning. Structurally: the ratchet in tools/check.sh reads this
 * binary and requires the assembler and both seam references to be gone while the two entry
 * points remain, because od_core_reset() and dispatch name them.
 *
 * The seam is deliberately linked here and deliberately never called. A stub that were absent
 * would make "calls no seam" true by link failure rather than by behaviour.
 */

#include "od_nfc.h"
#include "od_nfc_app.h"
#include "od_log.h"
#include "od_cmd_test_ctx.h"
#include "od_reply.h"
#include "od_sec2_app.h"
#include "od_txq.h"
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

static unsigned g_sent_n;
static unsigned g_seam_calls;
static unsigned g_log_calls;

void _od_log(int level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
    ++g_log_calls;
}

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
    (void)origin; (void)tag; (void)frame; (void)len;
    ++g_sent_n;
    return OD_RADIO_SENT;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{ (void)origin; (void)tag; return true; }

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{ (void)rp; (void)len; (void)why; }

/* Present, and expected never to run. */
bool od_nfc_app_read(uint8_t *type, uint8_t *data, uint16_t *len_io, uint16_t cap)
{
    (void)type; (void)data; (void)len_io; (void)cap;
    ++g_seam_calls;
    return false;
}

bool od_nfc_app_write(uint8_t type, const uint8_t *data, uint16_t len)
{
    (void)type; (void)data; (void)len;
    ++g_seam_calls;
    return false;
}

/* The od_sec2_app target hooks: an unclaimed device, so od_reply never seals. */
uint32_t od_sec2_app_now_ms(void) { return 1000u; }
void od_sec2_app_uid(uint8_t uid[8]) { memcpy(uid, UID, 8u); }
uint8_t od_sec2_app_dflags(void) { return 0u; }
const struct SecurityConfig *od_sec2_app_security(void) { return NULL; }

int main(void)
{
    /* Every sub-command, plus an unknown one and an empty body: the arm has no cases, so silence
     * has to hold for all of them rather than for the ones a machine would have recognised. */
    static const uint8_t subs[7] = { NFC_SUB_READ, NFC_SUB_WRITE, NFC_SUB_WRITE_START,
                                     NFC_SUB_WRITE_DATA, NFC_SUB_WRITE_END, 0x02u, 0xFFu };
    od_tx_reservation_t r;

    CASE("every sub-command answers nothing and returns UNKNOWN");
    for (unsigned i = 0; i < 7u; ++i) {
        uint8_t body[8] = { subs[i], OD_NFC_REC_TEXT, 0u, 4u, 1u, 2u, 3u, 4u };

        od_txq_reset();
        g_sent_n = 0u;
        CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
        {
            od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u }, &r, 10u, false);

            CHECK(od_nfc_frame(&ctx, od_span_make(body, sizeof body)) == OD_CMD_UNKNOWN);
        }
        od_txq_release(&r);
        CHECK(od_txq_process() == 0u);
        CHECK(g_sent_n == 0u);
    }

    CASE("an empty body is silent too");
    od_txq_reset();
    g_sent_n = 0u;
    CHECK(od_txq_reserve(1u, &r) == OD_TXQ_OK);
    {
        od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u }, &r, 2u, false);

        CHECK(od_nfc_frame(&ctx, od_span_none()) == OD_CMD_UNKNOWN);
    }
    od_txq_release(&r);
    CHECK(g_sent_n == 0u);

    CASE("the reset is callable and does nothing");
    od_nfc_reset();

    CASE("lifecycle reporting is compiled out with the capability");
    od_nfc_log_event(OD_NFC_LOG_PAYLOAD_SET_FAILED, -1);
    od_nfc_log_event(OD_NFC_LOG_EMULATION_START_FAILED, -2);
    od_nfc_log_event(OD_NFC_LOG_CONFIG_ABSENT, 0);
    od_nfc_log_event(OD_NFC_LOG_CONFIG_DISABLED, 0);
    od_nfc_log_event(OD_NFC_LOG_IC_UNSUPPORTED, 9);
    od_nfc_log_event(OD_NFC_LOG_T2T_SETUP_FAILED, -3);
    od_nfc_log_event(OD_NFC_LOG_T2T_ACTIVE, 1);
    CHECK(g_log_calls == 0u);

    CASE("no sub-command reached the tag seam");
    CHECK(g_seam_calls == 0u);

    printf("nfc_off: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
