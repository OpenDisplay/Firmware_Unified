#include "od_cmd_test_ctx.h"
#include "od_pipe.h"
#include "od_reply.h"
#include "od_xfer_internal.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static unsigned g_replies;
static uint16_t g_len;
static uint8_t g_frame[8];

#define CHECK(cond) do {                                                        \
    ++g_checks;                                                                \
    if (!(cond)) {                                                             \
        ++g_failures;                                                          \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                \
    }                                                                          \
} while (0)

od_txq_status_t od_reply(od_tx_reservation_t *r, const od_reply_t *rp,
                         const uint8_t *frame, uint16_t len)
{
    (void)r;
    (void)rp;
    (void)frame;
    (void)len;
    return OD_TXQ_INVARIANT;
}

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    (void)r;
    (void)rp;
    ++g_replies;
    g_len = len;
    if (len <= sizeof g_frame) {
        memcpy(g_frame, frame, len);
    }
    return OD_TXQ_OK;
}

/* No session in this fixture, so a refusal is the plaintext reply. */
od_txq_status_t od_reply_refusal(od_tx_reservation_t *r, const od_reply_t *rp,
                                 const uint8_t *frame, uint16_t len)
{
    return od_reply_plain(r, rp, frame, len);
}

int main(void)
{
    od_tx_reservation_t reservation;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u },
                                        &reservation, 2u, false);
    const uint8_t expected[] = { 0xFFu, 0x80u, 0x04u, 0x00u };
    char suffix[8] = "dirty";

    memset(&reservation, 0, sizeof reservation);
    CHECK(od_pipe_start(&ctx, od_span_none()) == OD_CMD_NACK);
    CHECK(g_replies == 1u && g_len == sizeof expected);
    CHECK(memcmp(g_frame, expected, sizeof expected) == 0);
    CHECK(od_pipe_data(&ctx, od_span_none()) == OD_CMD_UNKNOWN);
    CHECK(od_pipe_end(&ctx, od_span_none()) == OD_CMD_UNKNOWN);
    CHECK(g_replies == 1u);
    CHECK(od_pipe_log_suffix(suffix, sizeof suffix) == 0u && suffix[0] == '\0');

    printf("pipe_off: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
