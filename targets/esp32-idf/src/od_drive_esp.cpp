/* CMD_DRIVE_WRITE_START (0x0085) and CMD_SPI_RAW (0x0084) on ESP32.
 *
 * Both run od_epd through od_epd_cfg, beside bb_epaper rather than replacing it: ordinary uploads,
 * partials and the boot screen are untouched, and a device whose config carries no drive packets
 * never reaches this file. The bytes go out through bb_epaper's IDF backend on a BBEPDISP of our
 * own, so the shared SPI bus has one owner and the global `bbep` keeps its panel geometry.
 *
 * Panels on FastEPD or the IT8951 are refused: they have no SPI command bus to drive.
 */
#include "od_drive_esp.h"

#include <bb_epaper.h>
#include <string.h>

#include "display_service.h"
#include "od_drive_app.h"
#include "od_epd_cfg.h"
#include "od_hal_gpio.h"
#include "od_hal_sleep.h"
#include "od_hal_time.h"
#include "od_log.h"
#include "od_sensor_sht40.h"
#include "od_spi_raw_app.h"
#include "od_watchdog_app.h"
#include "opendisplay_protocol.h"
#include "structs.h"

extern struct od_config globalConfig;
void pwrmgm(bool onoff);
void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS,
                uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed);
void bbepWriteCmd(BBEPDISP *pBBEP, uint8_t cmd);
void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen);
void bbepDeInitIO(void);

/* bb_epaper's ESP32 rate for every panel on this target. */
#define DRIVE_SPI_HZ 8000000u

static BBEPDISP s_io;
static bool s_bus_up;
static bool s_spi_raw_open;
static bool s_drive_active;
static od_epd_cfg_stream_t s_stream;

/* --- bus ------------------------------------------------------------------------------------ */

static void bus_cmd(uint8_t c)
{
    bbepWriteCmd(&s_io, c);
}

static void bus_data(const uint8_t *d, size_t n)
{
    bbepWriteData(&s_io, (uint8_t *)(uintptr_t)d, (int)n);
}

static void bus_reset(uint32_t hold_ms)
{
    const uint8_t rst = globalConfig.displays[0].reset_pin;

    if (rst == OD_PIN_UNUSED) {
        return;
    }
    od_hal_gpio_write(rst, true);
    od_hal_delay_ms(hold_ms);
    od_hal_gpio_write(rst, false);
    od_hal_delay_ms(hold_ms);
    od_hal_gpio_write(rst, true);
    od_hal_delay_ms(hold_ms);
}

static int bus_busy_read(void)
{
    const uint8_t busy = globalConfig.displays[0].busy_pin;

    return busy == OD_PIN_UNUSED ? -1 : (od_hal_gpio_read(busy) != 0 ? 1 : 0);
}

static void bus_delay_ms(uint32_t ms)
{
    od_hal_delay_ms(ms);
}

static const od_epd_cfg_bus_t s_bus = {
    .cmd = bus_cmd,
    .data = bus_data,
    .reset = bus_reset,
    .busy_read = bus_busy_read,
    .delay_ms = bus_delay_ms,
    .now_ms = od_hal_uptime_ms,
    .faulted = NULL,
};

static void drive_phase(bool refreshing)
{
    od_watchdog_app_phase(refreshing ? OD_WDT_PHASE_REFRESH_WAIT : OD_WDT_PHASE_STREAM);
}

/* --- od_drive_app seam ---------------------------------------------------------------------- */

extern "C" const struct DisplayConfig *od_drive_app_display(void)
{
    return globalConfig.display_count > 0u ? &globalConfig.displays[0] : nullptr;
}

extern "C" const od_epd_cfg_bus_t *od_drive_app_bus(void)
{
    return &s_bus;
}

extern "C" bool od_drive_app_panel_up(const struct DisplayConfig *d)
{
    if (d == nullptr || fastepd_driver_used()) {
        return false;
    }
    /* The bb_epaper session may be warm; it must not keep the bus while od_epd drives it. */
    epdSessionForceOff();
    od_watchdog_app_phase(OD_WDT_PHASE_ACQUIRE_COLD);
    pwrmgm(true);
    memset(&s_io, 0, sizeof(s_io));
    /* bbepWriteCmd wakes a controller it thinks is asleep, which would run bb_epaper's own
     * reset and init in the middle of an od_epd sequence. */
    s_io.is_awake = 1;
    bbepInitIO(&s_io, d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin,
               DRIVE_SPI_HZ);
    if (d->busy_pin != OD_PIN_UNUSED) {
        const bool idle_high = od_epd_cfg_busy_idle_level() != 0u;

        od_hal_gpio_config_input(d->busy_pin, idle_high, !idle_high);
    }
    s_bus_up = true;
    od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
    return true;
}

extern "C" void od_drive_app_panel_down(void)
{
    if (!s_bus_up) {
        return;
    }
    s_bus_up = false;
    bbepDeInitIO();
    pwrmgm(false);
}

extern "C" void od_drive_app_abort(void)
{
    if (s_spi_raw_open) {
        opendisplay_display_spi_raw_close();
    }
    od_drive_esp_clear();
}

extern "C" bool od_drive_app_adopt(const od_color_geometry_t *frame, uint8_t passes,
                                   bool compressed, od_span_t tail)
{
    if (!od_epd_cfg_stream_init(&s_stream, frame, passes, compressed, tail.p,
                                (uint16_t)tail.n)) {
        return false;
    }
    s_drive_active = true;
    return true;
}

/* The SHT40 when one is configured; otherwise no reading, which skips temp_policy. */
extern "C" int8_t od_drive_app_temp_c(void)
{
    int t = 0;

    if (!od_sensor_sht40_read_temp_c(&globalConfig, &t) || t < -128 || t > 126) {
        return OD_DRIVE_TEMP_UNKNOWN;
    }
    return (int8_t)t;
}

/* --- transfer hooks (display_service.cpp) --------------------------------------------------- */

bool od_drive_esp_active(void)
{
    return s_drive_active;
}

bool od_drive_esp_write(uint32_t stream_offset, const uint8_t *data, size_t len)
{
    od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
    return s_drive_active &&
           od_epd_cfg_stream_write(&s_stream, stream_offset, data, len, drive_phase);
}

bool od_drive_esp_refresh(void)
{
    bool ok;

    if (!s_drive_active) {
        return false;
    }
    od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);
    ok = od_epd_cfg_finish();
    od_drive_esp_clear();
    return ok;
}

void od_drive_esp_clear(void)
{
    if (s_drive_active || s_bus_up) {
        od_epd_cfg_sleep();
    }
    s_drive_active = false;
    memset(&s_stream, 0, sizeof(s_stream));
    od_drive_app_panel_down();
}

/* --- CMD_SPI_RAW ----------------------------------------------------------------------------
 *
 * The session owns the panel: OPEN aborts any transfer, and the link-loss path closes it, so
 * bytes never go to an unpowered panel. Needs drive packets in the config, as on BG22: without
 * them there is no BUSY sense and no init sequence. */

extern "C" bool opendisplay_display_spi_raw_is_open(void)
{
    return s_spi_raw_open;
}

extern "C" int opendisplay_display_spi_raw_open(bool run_panel_init)
{
    const struct DisplayConfig *d = od_drive_app_display();

    od_drive_app_abort();
    if (d == nullptr || !od_epd_cfg_load(d, &s_bus)) {
        return -OD_ERR_SPI_RAW_NO_CONFIG;
    }
    if (!od_drive_app_panel_up(d)) {
        return -OD_ERR_SPI_RAW_PANEL;
    }
    if (run_panel_init && (!od_epd_cfg_select(0u, nullptr) || !od_epd_cfg_begin_pass(0u))) {
        od_log_warn("spi_raw: panel init failed rc=%d", od_epd_cfg_last_rc());
        od_drive_app_panel_down();
        return -OD_ERR_SPI_RAW_PANEL;
    }
    s_spi_raw_open = true;
    return 0;
}

extern "C" int opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    if (data == nullptr || len == 0u) {
        return -OD_ERR_SPI_RAW_BAD_ARGS;
    }
    od_epd_cfg_raw_cmd(data, len);
    return 0;
}

extern "C" int opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    if (data == nullptr || len == 0u) {
        return -OD_ERR_SPI_RAW_BAD_ARGS;
    }
    od_epd_cfg_raw_data(data, len);
    return 0;
}

extern "C" int opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out,
                                                bool *still_busy_out)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    if (timeout_ms > 90000u) {
        timeout_ms = 90000u;
    }
    od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);
    od_epd_cfg_raw_busy(timeout_ms, elapsed_ms_out, still_busy_out);
    return 0;
}

extern "C" int opendisplay_display_spi_raw_reset(uint8_t hold_ms)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    od_epd_cfg_raw_reset(hold_ms < 1u ? 1u : (hold_ms > 50u ? 50u : hold_ms));
    return 0;
}

extern "C" void opendisplay_display_spi_raw_close(void)
{
    if (!s_spi_raw_open) {
        return;
    }
    s_spi_raw_open = false;
    od_epd_cfg_sleep();
    od_drive_app_panel_down();
}
