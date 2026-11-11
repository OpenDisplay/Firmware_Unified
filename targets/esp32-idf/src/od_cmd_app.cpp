/* od_cmd_app.cpp -- this target's implementation of shared/core/od_cmd_app.h.
 *
 * ONE FUNCTION PER TARGET-OWNED OPCODE, and no switch. Promoted transfer opcodes route straight
 * to their shared machines. A capability represented here still gets a definition, so an
 * incomplete target-specific opcode addition is a link error.
 *
 * The predicates below (mutates-config, allow-unauthenticated) are questions the dispatcher asks
 * ABOUT an opcode before any handler runs, so they stay on od_dispatch.h rather than moving here.
 */

#include "od_claim_mode.h"
#include "od_reply.h"
#include "od_cmd_app.h"

#include "buzzer_control.h"
#include "communication.h"
#include "device_control.h"
#include "display_service.h"
#include "od_dispatch.h"

#include "od_cmd_reply.h"
#include "od_hal_sleep.h"
#include "od_log.h"

/* C++ linkage, matching their definitions -- these are target functions, not a C ABI. */
void reboot();
void enterDFUMode();
od_cmd_result_t handleClearConfig(const od_cmd_ctx_t *ctx);

/* The handlers still take (uint8_t*, uint16_t). Casting away const at the seam rather than
 * changing ~15 more signatures keeps this step to the routing change; they do not write through
 * it, which is checked. */
static inline uint8_t *bytes(od_span_t body) { return (uint8_t *)(uintptr_t)body.p; }
static inline uint16_t count(od_span_t body) { return (uint16_t)body.n; }

/* ------------------------------------------------------------------- device and lifecycle --- */

extern "C" od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)ctx; (void)body;
    /* Replies by not returning. The delay is the shipped one: it exists so a queued response to an
     * EARLIER command has a chance to leave before the reset. */
    (void)od_claim_mode_before_reboot();   /* OS2C, or END(CLAIMED); disarms the count */
    od_hal_delay_ms(100);
    reboot();
    return OD_CMD_OK;
}

extern "C" od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleFirmwareVersion(ctx);
}

extern "C" od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleReadMSD(ctx);
}

extern "C" od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const uint8_t ok[] = { 0x00u, RESP_ENTER_DFU };

    (void)body;
    (void)od_reply(ctx->r, &ctx->rp, ok, sizeof(ok));
    enterDFUMode();
    return OD_CMD_OK;
}

extern "C" od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handlePowerOffCommand(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleDeepSleepCommand(ctx, bytes(body), count(body));
}

/* ------------------------------------------------------------------------------- config --- */

extern "C" od_cmd_result_t od_cmd_app_config_read(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleReadConfig(ctx);
}

extern "C" od_cmd_result_t od_cmd_app_config_write(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleWriteConfig(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_config_chunk(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleWriteConfigChunk(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_config_clear(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return handleClearConfig(ctx);
}

/* -------------------------------------------------------------------------- peripherals --- */

extern "C" od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleLedActivate(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleLedStop(ctx, bytes(body), count(body));
}

extern "C" od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
    return handleBuzzerActivate(ctx, bytes(body), count(body));
}

/* ------------------------------------------------------------------ dispatcher predicates --- */

extern "C" bool od_cmd_mutates_config(uint16_t cmd)
{
    /* The set a live CONFIG_READ must exclude. It is target-side because the opcode set differs
     * per target -- Nordic has NFC, Silabs has no PIPE -- and because "mutates stored config" is a
     * statement about this firmware's handlers, not about the wire.
     *
     * CONFIG_READ itself is NOT here: the dispatcher tests for it separately, since a second read
     * is excluded for a different reason (it would restart a producer that has already promised
     * the host a chunk count). */
    switch (cmd) {
    case CMD_CONFIG_WRITE:   /* 0x0041 -- saveConfig + reloadConfigAfterSave */
    case CMD_CONFIG_CHUNK:   /* 0x0042 -- same, on completion */
    case CMD_CONFIG_CLEAR:   /* 0x0045 -- clearStoredConfig */
        return true;
    default:
        return false;
    }
}

