/* Image-store commands (0x0047-0x004B) on the XIAO nRF52840 module's flash.
 *
 * PRELOAD arms the ordinary upload path, as on BG22. The flash is on the always-on 3V3, so it is
 * woken for each operation and returned to deep power-down, which is worth ~10 uA at idle.
 *
 * The trigger/action table (0x004C/0x004D) shares this capability but is not implemented on this
 * target, so both NACK.
 */
#include "od_cmd_flash.h"

#include "od_cmd_app.h"
#include "od_color.h"
#include "od_hal_spi.h"
#include "od_hal_time.h"
#include "od_imgstore.h"
#include "od_log.h"
#include "od_reply.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_protocol.h"

#include <string.h>

#define NOR_CMD_RELEASE_PD 0xABu
#define NOR_CMD_POWER_DOWN 0xB9u

/* Module wiring, used as plain SPI: IO0 P0.20, IO1 P0.24, SCK P0.21, CSN P0.25. */
static const od_hal_spi_pins_t s_pins = { .mosi = 0x14u, .miso = 0x18u, .sck = 0x15u, .cs = 0x19u };

static bool s_armed;
static uint8_t s_armed_slot;
static bool s_awake;

void od_flash_store_wake(void)
{
    const uint8_t cmd = NOR_CMD_RELEASE_PD;

    if (s_awake || od_hal_spi_open(&s_pins) != OD_HAL_SPI_OK) {
        return;
    }
    (void)od_hal_spi_xfer(&cmd, 1u, NULL, 0u);
    od_hal_delay_us(50u);
    s_awake = true;
}

void od_flash_store_sleep(void)
{
    const uint8_t cmd = NOR_CMD_POWER_DOWN;

    if (!s_awake) {
        return;
    }
    (void)od_hal_spi_xfer(&cmd, 1u, NULL, 0u);
    od_hal_spi_close();
    s_awake = false;
}

void od_flash_store_mount(void)
{
    const struct od_config *cfg = opendisplay_get_global_config();
    const struct DisplayConfig *d;
    od_color_geometry_t g;

    if (od_imgstore_status()->mounted) {
        od_flash_store_wake();
        od_imgstore_unmount();
        s_awake = false;
    }
    if (cfg == NULL || !cfg->loaded || cfg->display_count == 0u) {
        return;
    }
    d = &cfg->displays[0];
    if (od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height, &g) !=
        OD_COLOR_OK) {
        return;
    }
    /* mount wakes the part itself and leaves the bus open. */
    if (od_imgstore_mount(&s_pins,
                          ((uint32_t)d->pixel_width << 16) ^ ((uint32_t)d->pixel_height << 4) ^
                              (uint32_t)d->color_scheme,
                          g.total_bytes)) {
        s_awake = true;
        od_flash_store_sleep();
    } else {
        s_awake = true;
        od_flash_store_sleep();
        od_log_warn("flash: image store not available");
    }
}

bool od_cmd_flash_take_arm(uint8_t *slot_out)
{
    if (!s_armed) {
        return false;
    }
    /* Consumed, so a surviving arm cannot divert the next image away from the panel. */
    s_armed = false;
    if (slot_out != NULL) {
        *slot_out = s_armed_slot;
    }
    return true;
}

void od_cmd_flash_clear_arm(void)
{
    s_armed = false;
}

bool od_flash_store_begin(uint8_t slot, uint16_t w, uint16_t h, uint8_t scheme, uint32_t len)
{
    od_flash_store_wake();
    if (!od_imgstore_begin(slot, w, h, scheme, 1u, len)) {
        od_flash_store_sleep();
        return false;
    }
    return true;
}

bool od_flash_store_finish(void)
{
    uint32_t digest = 0u;
    const bool ok = od_imgstore_finish(&digest);

    od_flash_store_sleep();
    return ok;
}

void od_flash_store_abort(void)
{
    od_imgstore_abort();
    od_flash_store_sleep();
}

static od_cmd_result_t nack(const od_cmd_ctx_t *ctx, uint16_t op)
{
    const uint8_t r[2] = { RESP_NACK, (uint8_t)op };

    (void)od_reply(ctx->r, &ctx->rp, r, sizeof r);
    return OD_CMD_NACK;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

od_cmd_result_t od_cmd_app_flash_info(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_imgstore_info_t *st = od_imgstore_status();
    uint8_t r[12];

    (void)body;
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_FLASH_INFO;
    r[2] = st->mounted ? 1u : 0u;
    r[3] = st->slot_count;
    put32(&r[4], st->slot_stride);
    put32(&r[8], st->capacity);
    return od_reply(ctx->r, &ctx->rp, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_list(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_imgstore_info_t *st = od_imgstore_status();
    uint8_t r[3u + (OD_IMGSTORE_MAX_SLOTS * 6u)];
    uint8_t n = 0u;

    (void)body;
    if (!st->mounted) {
        return nack(ctx, CMD_FLASH_LIST);
    }
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_FLASH_LIST;
    od_flash_store_wake();
    for (uint8_t i = 0u; i < st->slot_count && i < OD_IMGSTORE_MAX_SLOTS; i++) {
        od_imgstore_slot_t s;
        uint8_t *e = &r[3u + ((uint32_t)n * 6u)];

        if (!od_imgstore_slot(i, &s)) {
            break;
        }
        e[0] = i;
        e[1] = s.state;
        put32(&e[2], s.digest);
        n++;
    }
    od_flash_store_sleep();
    r[2] = n;
    return od_reply(ctx->r, &ctx->rp, r, (uint16_t)(3u + ((uint32_t)n * 6u))) == OD_TXQ_OK
               ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_erase(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[3];
    bool ok;

    if (!od_span_valid(body) || body.n < 1u) {
        return nack(ctx, CMD_FLASH_ERASE);
    }
    od_flash_store_wake();
    ok = body.p[0] == 0xFFu ? od_imgstore_erase_all() : od_imgstore_erase(body.p[0]);
    od_flash_store_sleep();
    r[0] = ok ? RESP_ACK : RESP_NACK;
    r[1] = (uint8_t)CMD_FLASH_ERASE;
    r[2] = body.p[0];
    (void)od_reply(ctx->r, &ctx->rp, r, sizeof r);
    return ok ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_preload(const od_cmd_ctx_t *ctx, od_span_t body)
{
    const od_imgstore_info_t *st = od_imgstore_status();
    uint8_t r[3];

    if (!od_span_valid(body) || body.n < 1u || !st->mounted || body.p[0] >= st->slot_count) {
        return nack(ctx, CMD_FLASH_PRELOAD);
    }
    s_armed = true;
    s_armed_slot = body.p[0];
    r[0] = RESP_ACK;
    r[1] = (uint8_t)CMD_FLASH_PRELOAD;
    r[2] = body.p[0];
    return od_reply(ctx->r, &ctx->rp, r, sizeof r) == OD_TXQ_OK ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_flash_display(const od_cmd_ctx_t *ctx, od_span_t body)
{
    uint8_t r[3];
    bool ok;

    if (!od_span_valid(body) || body.n < 1u) {
        return nack(ctx, CMD_FLASH_DISPLAY);
    }
    /* Acknowledged before the refresh, which outlasts the host's command timeout. */
    ok = od_display_from_slot_arm(body.p[0]);
    r[0] = ok ? RESP_ACK : RESP_NACK;
    r[1] = (uint8_t)CMD_FLASH_DISPLAY;
    r[2] = body.p[0];
    (void)od_reply(ctx->r, &ctx->rp, r, sizeof r);
    return ok ? OD_CMD_OK : OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_action_info(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, CMD_ACTION_INFO);
}

od_cmd_result_t od_cmd_app_action_fire(const od_cmd_ctx_t *ctx, od_span_t body)
{
    (void)body;
    return nack(ctx, CMD_ACTION_FIRE);
}
