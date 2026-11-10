/* ESP32 glue for the shared config-driven display path (shared/core/od_display.h).
 *
 * Every SPI panel command comes from the config's drive packets; a config without them draws
 * nothing and says "needs migration". What stays here is this target's own: the panel rail and
 * its keep-alive window (pwrmgm, AXP2101), the SPI2 bus (od_epd_esp), the watchdog breadcrumbs,
 * CMD_SPI_RAW, and the boot policy (safe mode, CLEAR_ON_BOOT, the presence probe, the deferred
 * boot screen after a config write). Panels driven by a built-in controller driver (IT8951,
 * FastEPD parallel) never reach od_display; displayUsesNativeDriver() routes them in
 * display_service.cpp.
 */
#include "od_display_esp.h"

#include <string.h>

#include "boot_screen.h"
#include "display_service.h"
#include "encryption_state.h"
#include "od_boot_screen.h"
#include "od_display.h"
#include "od_display_app.h"
#include "od_drive_app.h"
#include "od_epd_cfg.h"
#include "od_epd_esp.h"
#include "od_hal_gpio.h"
#include "od_hal_sleep.h"
#include "od_hal_time.h"
#include "od_ident_app.h"
#include "od_log.h"
#include "od_sensor_sht40.h"
#include "od_spi_raw_app.h"
#include "od_txq.h"
#include "od_watchdog_app.h"
#include "od_xfer.h"
#include "opendisplay_protocol.h"
#include "structs.h"
#include "touch_input.h"
#if defined(OPENDISPLAY_FASTEPD)
#include "display_fastepd.h"
#endif

extern struct od_config globalConfig;
extern volatile uint8_t pwrmgmState;
extern uint32_t pwrmgmOffDeadlineMs;
void pwrmgm(bool onoff);

/* After the sleep sequence, before the rail drops: the boost caps and VCOM need time to bleed.
 * 50 ms is too short on the 4.2in SSD16xx (EP42B): the image greys over the following minutes.
 * The discharge commands themselves are in that panel's 0xB1 sleep sequence. */
static const uint32_t kPostSleepBleedMs = 200u;
/* After a hardware reset every controller reports BUSY idle within milliseconds. */
static const uint32_t kBootProbeTimeoutMs = 2000u;

static bool s_bleed_pending;
static bool s_spi_raw_open;
static bool s_boot_applied;
static bool s_boot_pending;

static const struct DisplayConfig *display_cfg(void)
{
    return globalConfig.display_count > 0u ? &globalConfig.displays[0] : nullptr;
}

/* --- panel rail and keep-alive -------------------------------------------------------------- */

static bool axp2101_configured(void)
{
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        if (globalConfig.sensors[i].sensor_type == OD_SENSOR_TYPE_AXP2101) {
            return true;
        }
    }
    return false;
}

/* screen_timeout_seconds, clamped to EPD_KEEPALIVE_MAX_S. 0 powers the panel straight down.
 * Forced to 0 on AXP2101 boards (PMIC warm idle draw unmeasured). */
static uint32_t keep_alive_window_ms(void)
{
    uint8_t s = globalConfig.power_option.screen_timeout_seconds;

    if (axp2101_configured()) {
        return 0u;
    }
    if (s > EPD_KEEPALIVE_MAX_S) {
        s = EPD_KEEPALIVE_MAX_S;
    }
    return (uint32_t)s * 1000u;
}

/* Rail up, or a warm rail taken back to ACTIVE. pwrmgm(true) asserts the rail, waits 800 ms, runs
 * the AXP2101 bring-up and drives the panel lines to their idle levels. */
static void rail_on(void)
{
    if (pwrmgmState == PWR_OFF) {
        const uint32_t t0 = od_hal_uptime_ms();

        od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_RAIL);
        pwrmgm(true);
        od_log_debug("[EPD] rail up %u ms", (unsigned)(od_hal_uptime_ms() - t0));
    } else {
        pwrmgmState = PWR_ACTIVE;
        pwrmgmOffDeadlineMs = 0u;
    }
}

static void rail_off(void)
{
    if (pwrmgmState == PWR_OFF) {
        return;
    }
    od_watchdog_app_phase(OD_WDT_PHASE_FORCE_OFF);
    if (s_bleed_pending &&
        (globalConfig.system_config.pwr_pin != 0xFFu || axp2101_configured())) {
        od_hal_delay_ms(kPostSleepBleedMs);
    }
    s_bleed_pending = false;
    pwrmgm(false);
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE_OFF);
}

/* The keep-alive decision once the controller is asleep: warm for the configured window, or off
 * now. Never calls back into od_display, which is mid end_session when it gets here. */
static void rail_release(void)
{
    const uint32_t window = keep_alive_window_ms();

    if (pwrmgmState == PWR_OFF) {
        return;
    }
    if (window == 0u) {
        rail_off();
        return;
    }
    pwrmgmState = PWR_WARM;
    pwrmgmOffDeadlineMs = od_hal_uptime_ms() + window;
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE_WARM);
    od_log_info("[EPD session] release: rail warm, off in %u ms", (unsigned)window);
}

void displayRailAcquire(void)
{
    od_watchdog_app_phase(pwrmgmState == PWR_OFF ? OD_WDT_PHASE_ACQUIRE_COLD
                                                 : OD_WDT_PHASE_ACQUIRE_WARM);
    rail_on();
}

void displayRailRelease(bool success)
{
    if (!success || keep_alive_window_ms() == 0u) {
        epdSessionForceOff();
        return;
    }
    rail_release();
}

void epdSessionForceOff(void)
{
    if (s_spi_raw_open) {
        opendisplay_display_spi_raw_close();
    }
    if (od_display_active()) {
        od_display_abort();
    }
#if defined(OPENDISPLAY_FASTEPD)
    if (displayUsesNativeDriver() && pwrmgmState != PWR_OFF) {
        fastepd_direct_sleep();
        /* The rail is about to drop: the next push must fully re-init the controller. */
        fastepd_mark_hw_deinitialized();
    }
#endif
    od_epd_esp_close();
    if (pwrmgmState != PWR_OFF) {
        od_log_info("[EPD session] force off");
    }
    rail_off();
}

void epdSessionTick(void)
{
    if (pwrmgmState == PWR_WARM && (int32_t)(od_hal_uptime_ms() - pwrmgmOffDeadlineMs) >= 0) {
        od_log_info("[EPD session] keep-alive expired - powering panel off");
        epdSessionForceOff();
    }
}

bool epdSessionIsWarm(void)
{
    return pwrmgmState == PWR_WARM;
}

/* reTerminal Sticky: the MicroSD slot shares SPI2 with the panel (SCK 13 / MOSI 14 / CS 15).
 * Park the card -- SD_CS GPIO8 high, SD_EN GPIO10 low -- before any panel SPI, or an inserted
 * card can drive the bus. */
static void park_shared_sd_card(const struct DisplayConfig *d)
{
    if (d->clk_pin == 13u && d->data_pin == 14u && d->cs_pin == 15u) {
        od_hal_gpio_config_output(8, true);
        od_hal_gpio_config_output(10, false);
    }
}

/* --- od_display_app / od_drive_app / od_ident_app seams ------------------------------------- */

extern "C" void od_display_app_phase(od_display_phase_t phase)
{
    switch (phase) {
    case OD_DISPLAY_PHASE_INIT:
        od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
        break;
    case OD_DISPLAY_PHASE_STREAM:
        od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
        break;
    case OD_DISPLAY_PHASE_REFRESH:
        od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);
        break;
    case OD_DISPLAY_PHASE_IDLE:
    default:
        break;
    }
    /* Pass boundaries of a multi-pass drive transfer land here between refreshes, inside one
     * transfer write; feed so a 16-colour frame does not outlast the task watchdog. */
    od_watchdog_app_service();
}

extern "C" const struct DisplayConfig *od_drive_app_display(void)
{
    return display_cfg();
}

extern "C" const od_epd_cfg_bus_t *od_drive_app_bus(void)
{
    return od_epd_esp_bus();
}

extern "C" bool od_drive_app_panel_up(const struct DisplayConfig *d)
{
    if (d == nullptr || displayUsesNativeDriver()) {
        return false;
    }
    /* Never bring the rail up in watchdog safe mode: the one refusal point for the boot screen
     * and every pushed image alike. */
    if (od_watchdog_app_safe_mode()) {
        od_log_warn("panel: power-up refused - watchdog safe mode");
        return false;
    }
    park_shared_sd_card(d);
    displayRailAcquire();
    od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_BUS);
    if (!od_epd_esp_open(d)) {
        rail_off();
        return false;
    }
    s_bleed_pending = true;
    od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
    return true;
}

/* od_display has already sent the sleep sequence. The rail stays warm for the keep-alive window;
 * a failed refresh is forced off by the caller, which knows the result. */
extern "C" void od_drive_app_panel_down(void)
{
    od_epd_esp_close();
    rail_release();
}

extern "C" void od_drive_app_abort(void)
{
    if (s_spi_raw_open) {
        opendisplay_display_spi_raw_close();
    }
    displayXferClear(false);
}

extern "C" bool od_drive_app_adopt(const od_color_geometry_t *frame, uint8_t passes,
                                   bool compressed, od_span_t tail)
{
    return od_display_adopt_drive(frame, passes, compressed, tail);
}

/* The SHT40 when one is configured; otherwise no reading, which skips temp_policy. The die
 * sensor is not used: on this target it reads well above the glass. */
extern "C" int8_t od_drive_app_temp_c(void)
{
    int t = 0;

    if (!od_sensor_sht40_read_temp_c(&globalConfig, &t) || t < -128 || t > 126) {
        return OD_DRIVE_TEMP_UNKNOWN;
    }
    return (int8_t)t;
}

extern "C" const struct od_config *od_ident_app_config(void)
{
    return &globalConfig;
}

extern "C" bool od_ident_app_native_panel(const struct DisplayConfig *d)
{
    return displayConfigUsesNativeDriver(d);
}

/* --- boot screen ---------------------------------------------------------------------------- */

/* BUSY must reach its idle level within kBootProbeTimeoutMs of a reset, or the panel is absent or
 * silent and every later BUSY wait would run into its timeout. Not probed: split panels (two
 * controllers on one BUSY), no BUSY or RST pin, or a BUSY the 0xB1 says cannot be believed. */
static bool boot_panel_responds(const struct DisplayConfig *d)
{
    const od_epd_cfg_bus_t *bus = od_epd_esp_bus();
    uint32_t start;
    int idle;

    if (d->busy_pin == OD_PIN_UNUSED || d->reset_pin == OD_PIN_UNUSED ||
        (d->cs_pin_2 != OD_PIN_UNUSED && d->cs_pin_2 != d->cs_pin) ||
        !od_epd_cfg_load(d, bus) || !od_epd_cfg_busy_usable()) {
        return true;
    }
    if (!od_drive_app_panel_up(d)) {
        return true;   /* refused for another reason; od_display_boot reports it */
    }
    bus->reset(20u);
    idle = (int)od_epd_cfg_busy_idle_level();
    start = od_hal_uptime_ms();
    do {
        od_hal_delay_ms(10);
        if (bus->busy_read() == idle) {
            od_epd_esp_close();
            return true;
        }
    } while (od_hal_uptime_ms() - start < kBootProbeTimeoutMs);
    /* Raw rail cut: a sleep sequence would wait on the BUSY that just failed. */
    od_epd_esp_close();
    s_bleed_pending = false;
    rail_off();
    return false;
}

bool displayBootConfigDriven(void)
{
    const struct DisplayConfig *d = display_cfg();
    od_display_rc_t rc;

    s_boot_pending = false;
    if (s_boot_applied) {
        return true;
    }
    if (od_watchdog_app_safe_mode()) {
        od_log_warn("boot display skipped: watchdog safe mode");
        s_boot_applied = true;
        return true;
    }
    if (d == nullptr) {
        od_log_warn("No display found");
        return false;
    }
    if ((d->transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT) != 0u) {
        od_log_info("boot display intentionally skipped by CLEAR_ON_BOOT");
        s_boot_applied = true;
        return true;
    }
    /* Not latched: the first config write that brings drive packets shows the boot screen. */
    if (od_display_needs_migration()) {
        od_log_warn("boot display skipped: config has no drive packets (needs migration)");
        return true;
    }
    if (!boot_panel_responds(d)) {
        od_log_warn("Panel not responding (BUSY stays busy after reset) - skipping boot screen");
        s_boot_applied = true;
        return false;
    }
    touchSuspendForEpdRefresh();
    rc = od_display_boot(&globalConfig, &securityConfig, bootScreenBufs());
    epdSessionForceOff();   /* boot ends with the rail off, never warm */
    touchResumeAfterEpdRefresh();
    if (rc == OD_DISPLAY_OK) {
        s_boot_applied = true;
        od_log_info("boot display refresh complete");
        return true;
    }
    od_log_error("boot display: rc=%d", (int)rc);
    /* A config that cannot render one would fail identically next time. */
    if (rc != OD_DISPLAY_PANEL) {
        s_boot_applied = true;
    }
    return false;
}

void displayRequestBootAfterConfigWrite(void)
{
    if (!s_boot_applied && !displayUsesNativeDriver()) {
        s_boot_pending = true;
    }
}

void displayServiceDeferredBoot(void)
{
    if (!s_boot_pending || od_txq_depth() != 0u || od_xfer_active() || od_display_active() ||
        s_spi_raw_open || epdRefreshInProgress) {
        return;
    }
    if (!displayBootConfigDriven()) {
        od_log_error("boot display after config write failed");
    }
}

/* --- CMD_SPI_RAW ----------------------------------------------------------------------------
 *
 * The session owns the panel: OPEN aborts any transfer, and the link-loss path closes it, so
 * bytes never go to an unpowered panel. Needs drive packets in the config: without them there is
 * no BUSY sense and no init sequence. */

extern "C" bool opendisplay_display_spi_raw_is_open(void)
{
    return s_spi_raw_open;
}

extern "C" int opendisplay_display_spi_raw_open(bool run_panel_init)
{
    const struct DisplayConfig *d = display_cfg();

    od_drive_app_abort();
    if (d == nullptr || displayUsesNativeDriver() || !od_epd_cfg_load(d, od_epd_esp_bus())) {
        return -OD_ERR_SPI_RAW_NO_CONFIG;
    }
    if (!od_drive_app_panel_up(d)) {
        return -OD_ERR_SPI_RAW_PANEL;
    }
    /* run_panel_init stops at the image marker, so the caller owns everything after the
     * registers. Without it nothing is clocked out and virgin register state is observable. */
    if (run_panel_init && (!od_epd_cfg_select(0u, nullptr) || !od_epd_cfg_begin_pass(0u))) {
        od_log_warn("spi_raw: panel init failed rc=%d", od_epd_cfg_last_rc());
        od_epd_esp_close();
        rail_off();
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
    /* Clamped per the protocol: 0 is not a pulse, and a long hold from one byte is a mistake. */
    od_epd_cfg_raw_reset(hold_ms < 1u ? 1u : (hold_ms > 50u ? 50u : hold_ms));
    return 0;
}

/* Idempotent, so a host recovering from a dropped link can always get to a known state. */
extern "C" void opendisplay_display_spi_raw_close(void)
{
    if (!s_spi_raw_open) {
        return;
    }
    s_spi_raw_open = false;
    od_epd_cfg_sleep();
    od_epd_esp_close();
    rail_off();
}
