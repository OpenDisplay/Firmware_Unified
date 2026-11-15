/* Telink display glue over the shared config-driven path (shared/core/od_display.h).
 *
 * What is this target's own: the pin encoding, a bit-banged write-only SPI bus, the panel rail
 * and its settle times, and the transfer glue. Every panel command comes from the config's drive
 * packets; a config without them draws nothing and reports "needs migration".
 *
 * Pin bytes: od_tlsr_pins.h.
 *
 * Also the panel half of claim mode: the setup-family screens, the key-state redraw and the
 * 0x27 reset pin. */
#include "od_tlsr_sdk.h"

#include "od_tlsr_pins.h"
#include "od_tlsr_target.h"

#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "od_claim_app.h"
#include "od_claim_mode.h"
#include "od_config.h"
#include "od_display.h"
#include "od_display_app.h"
#include "od_drive_app.h"
#include "od_epd_cfg.h"
#include "od_hal_time.h"
#include "od_sec2_app.h"
#include "od_spi_raw_app.h"
#include "od_txq.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "opendisplay_protocol.h"

#include <string.h>

#define PIN_UNUSED 0xFFu

/* The rail needs time before the controller will talk; run the init against a controller that
 * is still coming up and BUSY reads idle, every wait returns at once, and the frame is clocked
 * into nothing. Same two settles as the other targets' rail bring-up: the rail, then the signal
 * lines at their idle levels before RST is toggled. */
#define RAIL_SETTLE_MS 800u
#define PIN_SETTLE_MS  100u

#define INFLATE_CHUNK 256u

static uint8_t s_dc = PIN_UNUSED;
static uint8_t s_cs = PIN_UNUSED;
static uint8_t s_rst = PIN_UNUSED;
static uint8_t s_busy = PIN_UNUSED;
static uint8_t s_clk = PIN_UNUSED;
static uint8_t s_mosi = PIN_UNUSED;

static bool s_boot_applied;
static bool s_spi_raw_open;
static bool s_boot_pending;
static uint8_t s_inflate[INFLATE_CHUNK];
/* One native row: 400 px at 4 bpp, or a 2 bpp row plus its plane row, covers this fleet's
 * glass. The QR module bitmap as on the other targets. */
static uint8_t s_boot_row[200];
static uint8_t s_boot_qr[256];

/* ---- pins ---- */

#define pin_valid od_tlsr_pin_valid
#define pin_sdk   od_tlsr_pin_sdk

/* PA7 is SWS, the only wired way back into a tag whose image is broken: never touched. */
#define PIN_SWS 7u

static void pin_out(uint8_t p, bool high)
{
    if (!pin_valid(p)) {
        return;
    }
    gpio_set_func(pin_sdk(p), AS_GPIO);
    gpio_write(pin_sdk(p), high ? 1u : 0u);
    gpio_set_input_en(pin_sdk(p), 0u);
    gpio_set_output_en(pin_sdk(p), 1u);
}

static void pin_in(uint8_t p, GPIO_PullTypeDef pull)
{
    if (!pin_valid(p)) {
        return;
    }
    gpio_set_func(pin_sdk(p), AS_GPIO);
    gpio_set_output_en(pin_sdk(p), 0u);
    gpio_setup_up_down_resistor(pin_sdk(p), pull);
    gpio_set_input_en(pin_sdk(p), 1u);
}

/* High-impedance, input buffer off: nothing back-powers an unpowered controller and nothing
 * leaks through a floating input. */
static void pin_park(uint8_t p)
{
    if (!pin_valid(p)) {
        return;
    }
    gpio_set_func(pin_sdk(p), AS_GPIO);
    gpio_set_output_en(pin_sdk(p), 0u);
    gpio_set_input_en(pin_sdk(p), 0u);
    gpio_setup_up_down_resistor(pin_sdk(p), PM_PIN_UP_DOWN_FLOAT);
}

static inline void pin_set(uint8_t p, bool high)
{
    if (pin_valid(p)) {
        gpio_write(pin_sdk(p), high ? 1u : 0u);
    }
}

/* ---- config ---- */

static const struct od_config *config(void)
{
    return od_tlsr_config_loaded() ? od_tlsr_config() : NULL;
}

static const struct DisplayConfig *display_cfg(void)
{
    const struct od_config *c = config();

    return (c == NULL || c->display_count == 0u) ? NULL : &c->displays[0];
}

/* ---- bus: bit-banged SPI mode 0, MSB first, write-only ---- */

static void epd_spi_write(const uint8_t *d, size_t n)
{
    const GPIO_PinTypeDef clk = pin_sdk(s_clk), mosi = pin_sdk(s_mosi);

    while (n-- > 0u) {
        uint8_t b = *d++;
        unsigned i;

        for (i = 0u; i < 8u; ++i) {
            gpio_write(mosi, (b & 0x80u) ? 1u : 0u);
            gpio_write(clk, 1u);
            b = (uint8_t)(b << 1);
            gpio_write(clk, 0u);
        }
    }
}

/* Three-wire read: the controller drives the data line while D/C is high and SCK is clocked.
 * One CS frame per byte, sampled after the rising edge, as the legacy firmware v108 reads (0xa4f8). */
static bool bus_read_bytes(uint8_t pin, uint8_t *dst, uint8_t n)
{
    const GPIO_PinTypeDef clk = pin_sdk(s_clk), din = pin_sdk(pin);
    uint8_t k;

    if (!pin_valid(pin) || !pin_valid(s_clk) || dst == NULL) {
        return false;
    }
    gpio_set_output_en(din, 0u);
    gpio_set_input_en(din, 1u);
    pin_set(s_dc, true);
    for (k = 0u; k < n; ++k) {
        uint8_t b = 0u;
        unsigned i;

        pin_set(s_cs, false);
        sleep_us(10u);
        for (i = 0u; i < 8u; ++i) {
            gpio_write(clk, 0u);
            gpio_write(clk, 1u);
            b = (uint8_t)((b << 1) | (gpio_read(din) ? 1u : 0u));
        }
        gpio_write(clk, 0u);
        pin_set(s_cs, true);
        dst[k] = b;
    }
    if (pin == s_mosi) {
        gpio_write(din, 0u);
        gpio_set_input_en(din, 0u);
        gpio_set_output_en(din, 1u);
    }
    return true;
}

static void bus_cmd(uint8_t c)
{
    pin_set(s_dc, false);
    pin_set(s_cs, false);
    epd_spi_write(&c, 1u);
    pin_set(s_cs, true);
    pin_set(s_dc, true);
}

/* A boot-screen plane is rendered row by row between data writes, with no BUSY wait or delay in
 * between: on this core the QR and text tests per pixel take longer than the 4 s watchdog for
 * one plane. So each write feeds the watchdog and, at most every 100 ms, ticks the power-cycle
 * count, so a render that starts before T_ARM still lets the factory-reset pattern count. */
static void bus_data(const uint8_t *d, size_t n)
{
    static uint32_t last_tick;
    const uint32_t now = od_hal_uptime_ms();

    wd_clear();
    if ((uint32_t)(now - last_tick) >= 100u) {
        last_tick = now;
        od_claim_mode_tick_count();
    }
    pin_set(s_cs, false);
    epd_spi_write(d, n);
    pin_set(s_cs, true);
}

/* Every BUSY poll and fixed delay of a pass lands here, on the superloop. The watchdog is fed
 * and claim mode's power-cycle count ticks in 100 ms slices, as on the other targets. The link
 * layer runs from interrupts, so the connection survives the wait; ATT work waits for it. */
static void bus_delay_ms(uint32_t ms)
{
    while (ms > 0u) {
        const uint32_t step = ms > 100u ? 100u : ms;
        uint32_t left = step;

        while (left > 0u) {
            const uint32_t s = left > 20u ? 20u : left;

            wd_clear();
            sleep_us(s * 1000u);
            left -= s;
        }
        ms -= step;
        od_claim_mode_tick_count();
    }
}

static void bus_reset(uint32_t hold_ms)
{
    if (!pin_valid(s_rst)) {
        return;
    }
    pin_set(s_rst, true);
    bus_delay_ms(hold_ms);
    pin_set(s_rst, false);
    bus_delay_ms(hold_ms);
    pin_set(s_rst, true);
    bus_delay_ms(hold_ms);
}

static int bus_busy_read(void)
{
    return pin_valid(s_busy) ? (gpio_read(pin_sdk(s_busy)) ? 1 : 0) : -1;
}

static bool bus_faulted(void)
{
    return false;
}

static bool bus_pin_write(uint8_t pin, uint8_t level)
{
    if (!pin_valid(pin) || level > 2u) {
        return false;
    }
    if (level == 2u) {
        pin_in(pin, PM_PIN_UP_DOWN_FLOAT);
    } else {
        pin_out(pin, level != 0u);
    }
    return true;
}

/* cs_select stays NULL (OD_CAP_DUAL_CS 0). */
static const od_epd_cfg_bus_t s_bus = {
    .cmd = bus_cmd,
    .data = bus_data,
    .reset = bus_reset,
    .busy_read = bus_busy_read,
    .delay_ms = bus_delay_ms,
    .now_ms = od_hal_uptime_ms,
    .faulted = bus_faulted,
    .pin_write = bus_pin_write,
    .read_bytes = bus_read_bytes,
};

/* ---- rail ---- */

static bool rail_active_low(const struct od_config *c)
{
    return (c->system_config.device_flags & OD_DEVICE_FLAG_PWR_PIN_ACTIVE_LOW) != 0u;
}

/* Off also pulls the pin toward its off level: deep-retention sleep keeps the analog pull
 * resistors but not the output driver, so the pull is what holds the rail off then. */
static void rail_set(const struct od_config *c, bool on)
{
    const uint8_t p = c->system_config.pwr_pin;
    const bool off_high = rail_active_low(c);

    pin_out(p, on != off_high);
    if (pin_valid(p)) {
        gpio_setup_up_down_resistor(pin_sdk(p), on ? PM_PIN_UP_DOWN_FLOAT
                                                   : off_high ? PM_PIN_PULLUP_10K
                                                              : PM_PIN_PULLDOWN_100K);
    }
}

static void panel_pins_park(const struct DisplayConfig *d)
{
    pin_park(d->cs_pin);
    pin_park(d->data_pin);
    pin_park(d->clk_pin);
    pin_park(d->dc_pin);
    pin_park(d->reset_pin);
    pin_park(d->busy_pin);
}

static bool panel_pins_ok(const struct DisplayConfig *d)
{
    return pin_valid(d->data_pin) && pin_valid(d->clk_pin) && pin_valid(d->cs_pin) &&
           pin_valid(d->dc_pin);
}

/* 0xFF pwr_pin: the rail is permanently powered, so the lines stay driven at idle. */
static void panel_power_down(void)
{
    const struct od_config *c = config();
    const struct DisplayConfig *d = display_cfg();

    if (c == NULL || d == NULL) {
        return;
    }
    if (pin_valid(c->system_config.pwr_pin)) {
        panel_pins_park(d);
        rail_set(c, false);
    } else {
        pin_out(d->clk_pin, false);
        pin_out(d->data_pin, false);
    }
}

/* ---- glass temperature: the controller's own sensor (TSC, 0x40), read at every panel-up.
 * the legacy firmware reads it the same way and uses the first byte as whole degrees C, signed. The MCU
 * has a temperature input on its ADC but no calibration for it, so it is not used. ---- */

#define TEMP_TTL_MS (60u * 60u * 1000u)

static bool s_temp_valid;
static int8_t s_temp_c;
static uint32_t s_temp_at;

static bool wait_idle_ms(bool idle_high, uint32_t ms)
{
    const uint32_t t0 = od_hal_uptime_ms();

    while ((gpio_read(pin_sdk(s_busy)) ? true : false) != idle_high) {
        if ((uint32_t)(od_hal_uptime_ms() - t0) >= ms) {
            return false;
        }
        wd_clear();
        sleep_us(1000u);
    }
    return true;
}

static void panel_read_temperature(bool idle_high)
{
    uint8_t b[2];

    if (!pin_valid(s_busy) || !pin_valid(s_rst)) {
        return;
    }
    bus_reset(10u);
    if (!wait_idle_ms(idle_high, 100u)) {
        return;
    }
    bus_cmd(0x40u);
    if (!wait_idle_ms(idle_high, 400u) || !bus_read_bytes(s_mosi, b, 2u)) {
        return;
    }
    /* A plausible glass reading only: a floating line reads 0x00 or 0xFF. */
    if ((int8_t)b[0] > -30 && (int8_t)b[0] < 70 && b[0] != 0u) {
        s_temp_c = (int8_t)b[0];
        s_temp_at = od_hal_uptime_ms();
        s_temp_valid = true;
    }
}

bool od_tlsr_display_temperature(int8_t *c)
{
    if (!s_temp_valid || (uint32_t)(od_hal_uptime_ms() - s_temp_at) > TEMP_TTL_MS) {
        return false;
    }
    *c = s_temp_c;
    return true;
}

static bool panel_power_up(const struct DisplayConfig *d)
{
    const struct od_config *c = config();
    const bool idle_high = od_epd_cfg_busy_idle_level() != 0u;

    if (c == NULL || d == NULL || !panel_pins_ok(d)) {
        return false;
    }
    if (pin_valid(c->system_config.pwr_pin)) {
        rail_set(c, true);
        bus_delay_ms(RAIL_SETTLE_MS);
    }
    s_dc = d->dc_pin;
    s_cs = d->cs_pin;
    s_rst = d->reset_pin;
    s_busy = d->busy_pin;
    s_clk = d->clk_pin;
    s_mosi = d->data_pin;
    pin_out(s_rst, true);
    pin_out(s_cs, true);
    pin_out(s_dc, true);
    pin_out(s_clk, false);
    pin_out(s_mosi, false);
    pin_in(s_busy, idle_high ? PM_PIN_PULLUP_1M : PM_PIN_PULLDOWN_100K);
    bus_delay_ms(PIN_SETTLE_MS);
    panel_read_temperature(idle_high);
    return true;
}

void od_tlsr_display_init(void)
{
    panel_power_down();
}

/* ---- od_display_app / od_drive_app ---- */

void od_display_app_phase(od_display_phase_t phase)
{
    (void)phase;
    wd_clear();
}

const struct DisplayConfig *od_drive_app_display(void)
{
    return display_cfg();
}

const od_epd_cfg_bus_t *od_drive_app_bus(void)
{
    return &s_bus;
}

void od_drive_app_abort(void)
{
    opendisplay_display_spi_raw_close();
    od_display_abort();
}

bool od_drive_app_panel_up(const struct DisplayConfig *d)
{
    if (!panel_power_up(d)) {
        panel_power_down();
        return false;
    }
    return true;
}

void od_drive_app_panel_down(void)
{
    panel_power_down();
}

bool od_drive_app_adopt(const od_color_geometry_t *frame, uint8_t passes, bool compressed,
                        od_span_t tail)
{
    return od_display_adopt_drive(frame, passes, compressed, tail);
}

/* Single-threaded superloop: nothing drives the panel behind the handler's back. */
bool od_drive_app_panel_busy(void) { return false; }

/* The glass reading from the last panel-up, while it is under an hour old. */
int8_t od_drive_app_temp_c(void)
{
    int8_t c;

    return od_tlsr_display_temperature(&c) ? c : OD_DRIVE_TEMP_UNKNOWN;
}

/* ---- od_boot_app, identity half (od_display.c is the panel half) ---- */

uint32_t od_boot_app_device_id24(void)
{
    uint8_t mac[6];

    od_tlsr_mac(mac);
    return ((uint32_t)mac[2] << 16) | ((uint32_t)mac[1] << 8) | mac[0];
}

void od_boot_app_firmware_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    *major = OD_FW_MAJOR;
    *minor = OD_FW_MINOR;
    *patch = OD_FW_PATCH;
}

float od_boot_app_battery_volts(void)
{
    return (float)od_tlsr_battery_mv() / 1000.0f;
}

float od_boot_app_chip_temp_c(void)
{
    return od_tlsr_chip_temperature_c();
}

static bool boot_apply(void)
{
    const struct DisplayConfig *d = display_cfg();
    const struct od_boot_bufs bufs = { s_boot_row, sizeof s_boot_row, s_boot_qr, sizeof s_boot_qr };
    od_display_rc_t rc;

    s_boot_pending = false;
    if (s_boot_applied) {
        return true;
    }
    if (d == NULL) {
        return false;
    }
    if ((d->transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT) != 0u) {
        s_boot_applied = true;
        return true;
    }
    /* After an unplanned reset since power-on, no boot screen until the next power-on: a render
     * that resets the chip would otherwise do so on every boot, before the radio or the
     * power-cycle count can reach it. */
    if (od_tlsr_hal_unplanned() != 0u) {
        s_boot_applied = true;
        return true;
    }
    /* Not latched: the first config write that brings drive packets shows the boot screen. */
    if (od_display_needs_migration()) {
        return true;
    }
    rc = od_display_boot(config(), od_sec2_app_security(), &bufs);
    s_boot_applied = rc == OD_DISPLAY_OK;
    return s_boot_applied;
}

void od_tlsr_display_request_boot(void)
{
    if (!s_boot_applied) {
        s_boot_pending = true;
    }
}

/* SECURITY_V2 7.8 / FACTORY_RESET.md 4 (od_claim_app.h): a setup-family screen through the
 * mode-0 pass, from od_claim_mode_tick() on the superloop. Blocks for the refresh. CLEAR_ON_BOOT
 * does not apply: the code has nowhere else to go. A drawn screen latches the boot screen, so a
 * config write in claim mode does not paint the normal screen over it. */
bool od_claim_app_draw(enum od_setup_screen kind, uint32_t code)
{
    const struct od_boot_bufs bufs = { s_boot_row, sizeof s_boot_row, s_boot_qr, sizeof s_boot_qr };

    if (kind == OD_SETUP_SCREEN_NORMAL || display_cfg() == NULL || s_spi_raw_open ||
        od_display_needs_migration()) {
        return false;
    }
    if (od_display_setup_screen(config(), kind, code, &bufs) != OD_DISPLAY_OK) {
        return false;
    }
    s_boot_applied = true;
    s_boot_pending = false;
    return true;
}

/* SECURITY_V2 10.7 (od_sec2_app.h): the key lines went stale, so the NORMAL boot screen is drawn
 * again from od_tlsr_display_service() once the link is idle. */
void od_sec2_app_key_screen_changed(void)
{
    if (od_claim_mode_active()) {
        return;
    }
    s_boot_applied = false;
    s_boot_pending = true;
}

/* The 0x27 reset pin, in the config pin encoding. PIN_SWS is refused, as for raw GPIO. */
bool od_claim_app_pin_input(uint8_t pin, bool pull_up, bool pull_down)
{
    if (!pin_valid(pin) || pin == PIN_SWS) {
        return false;
    }
    pin_in(pin, pull_up ? PM_PIN_PULLUP_10K : pull_down ? PM_PIN_PULLDOWN_100K : PM_PIN_UP_DOWN_FLOAT);
    return true;
}

int od_claim_app_pin_read(uint8_t pin)
{
    if (!pin_valid(pin) || pin == PIN_SWS) {
        return -1;
    }
    return gpio_read(pin_sdk(pin)) ? 1 : 0;
}

/* ---- CMD_SPI_RAW: the session owns the panel until CLOSE, a re-OPEN or a transfer abort
 * (which a BLE disconnect reaches), so bytes never go to an unpowered panel. Needs drive
 * packets: without them there is no BUSY sense and no init sequence. ---- */

/* Ports B and C keep input-enable in analog registers; reg_gpio_ie() (and the SDK's
 * gpio_is_input_en(), built on it) is only right for A, D and E. An input left enabled on a
 * floating pin leaks tens of uA in suspend. */
static bool input_enabled(GPIO_PinTypeDef g)
{
    const uint8_t bit = (uint8_t)(g & 0xFFu);

    switch (g & 0xF00) {
    case GPIO_GROUPB:
        return (analog_read(areg_gpio_pb_ie) & bit) != 0u;
    case GPIO_GROUPC:
        return (analog_read(areg_gpio_pc_ie) & bit) != 0u;
    default:
        return (reg_gpio_ie(g) & bit) != 0u;
    }
}

#if OD_TLSR_FAULT_TEST
/* Fault-injection build only (-DOD_TLSR_FAULT_TEST=ON): GPIO pin 0xFE arms a hang in the next
 * deep-retention wake, to prove that the watchdog reset it causes turns retention off. */
#define PIN_FAULT_ARM 0xFEu
static bool s_fault_armed;

bool od_tlsr_fault_armed(void)
{
    return s_fault_armed;
}
#endif

int opendisplay_display_spi_raw_gpio(uint8_t pin, uint8_t mode, uint8_t *level_out)
{
    const GPIO_PinTypeDef g = pin_sdk(pin);

#if OD_TLSR_FAULT_TEST
    if (pin == PIN_FAULT_ARM && level_out != NULL) {
        s_fault_armed = true;
        *level_out = 1u;
        return 0;
    }
#endif

    if (!pin_valid(pin) || pin == PIN_SWS || level_out == NULL) {
        return -(int)OD_ERR_SPI_RAW_BAD_ARGS;
    }
    switch (mode) {
    case OD_SPI_RAW_GPIO_READ:
        break;
    case OD_SPI_RAW_GPIO_IN_FLOAT:
        pin_in(pin, PM_PIN_UP_DOWN_FLOAT);
        break;
    case OD_SPI_RAW_GPIO_IN_PULLUP:
        pin_in(pin, PM_PIN_PULLUP_10K);
        break;
    case OD_SPI_RAW_GPIO_IN_PULLDOWN:
        pin_in(pin, PM_PIN_PULLDOWN_100K);
        break;
    case OD_SPI_RAW_GPIO_OUT_LOW:
    case OD_SPI_RAW_GPIO_OUT_HIGH:
        pin_out(pin, mode == OD_SPI_RAW_GPIO_OUT_HIGH);
        break;
    default:
        return -(int)OD_ERR_SPI_RAW_BAD_ARGS;
    }
    /* An output reads its pad too once input is enabled; enable it for the read only. */
    if (mode >= OD_SPI_RAW_GPIO_OUT_LOW || mode == OD_SPI_RAW_GPIO_READ) {
        const bool was = input_enabled(g);

        gpio_set_input_en(g, 1u);
        *level_out = gpio_read(g) ? 1u : 0u;
        if (!was) {
            gpio_set_input_en(g, 0u);
        }
    } else {
        *level_out = gpio_read(g) ? 1u : 0u;
    }
    return 0;
}

bool opendisplay_display_spi_raw_is_open(void)
{
    return s_spi_raw_open;
}

int opendisplay_display_spi_raw_open(bool run_panel_init)
{
    const struct DisplayConfig *d = display_cfg();

    od_drive_app_abort();
    if (d == NULL || !od_epd_cfg_load(d, &s_bus)) {
        return -OD_ERR_SPI_RAW_NO_CONFIG;
    }
    if (!od_drive_app_panel_up(d)) {
        return -OD_ERR_SPI_RAW_PANEL;
    }
    /* The init stops at the image marker, so the caller owns everything after the registers. */
    if (run_panel_init && (!od_epd_cfg_select(0u, NULL) || !od_epd_cfg_begin_pass(0u))) {
        od_drive_app_panel_down();
        return -OD_ERR_SPI_RAW_PANEL;
    }
    if (run_panel_init) {
        od_epd_cfg_raw_detach();
    }
    s_spi_raw_open = true;
    return 0;
}

int opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    if (data == NULL || len == 0u) {
        return -OD_ERR_SPI_RAW_BAD_ARGS;
    }
    od_epd_cfg_raw_cmd(data, len);
    return 0;
}

int opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    if (data == NULL || len == 0u) {
        return -OD_ERR_SPI_RAW_BAD_ARGS;
    }
    od_epd_cfg_raw_data(data, len);
    return 0;
}

int opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out,
                                     bool *still_busy_out)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    if (timeout_ms > 90000u) {
        timeout_ms = 90000u;
    }
    od_epd_cfg_raw_busy(timeout_ms, elapsed_ms_out, still_busy_out);
    return 0;
}

int opendisplay_display_spi_raw_reset(uint8_t hold_ms)
{
    if (!s_spi_raw_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    od_epd_cfg_raw_reset(hold_ms < 1u ? 1u : (hold_ms > 50u ? 50u : hold_ms));
    return 0;
}

void opendisplay_display_spi_raw_close(void)
{
    if (!s_spi_raw_open) {
        return;
    }
    s_spi_raw_open = false;
    od_epd_cfg_sleep();
    od_drive_app_panel_down();
}

/* On the superloop with the link idle: nothing queued, no transfer holding the panel. */
void od_tlsr_display_retention_wake(void)
{
    panel_power_down();
}

bool od_tlsr_display_idle(void)
{
    return !s_boot_pending && !s_spi_raw_open && !od_display_active();
}

void od_tlsr_display_service(void)
{
    if (!s_boot_pending || s_spi_raw_open || od_txq_depth() != 0u || od_xfer_active() ||
        od_display_active()) {
        return;
    }
    (void)boot_apply();
}

/* ---- od_xfer_app ---- */

void od_xfer_app_prepare_start(void)
{
    if (od_display_active()) {
        od_display_abort();
    }
}

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
    const struct DisplayConfig *d = display_cfg();
    bool geometry_ok;

    if (out == NULL || d == NULL) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->needs_migration = od_display_needs_migration();
    geometry_ok = od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                                           &out->geometry) == OD_COLOR_OK;
    if (!geometry_ok && !out->needs_migration) {
        return false;
    }
    out->width = d->pixel_width;
    out->height = d->pixel_height;
    out->partial_enabled = false; /* OD_CAP_PARTIAL 0 */
    return true;
}

bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
    od_display_rc_t rc;

    if (display_cfg() == NULL || geometry == NULL || geometry->total_bytes == 0u) {
        return false;
    }
    rc = od_display_begin_full(geometry);
    if (rc == OD_DISPLAY_PANEL_BUSY) {
        od_xfer_app_note_start_error(OD_ERR_PANEL_BUSY);
    }
    return rc == OD_DISPLAY_OK;
}

uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
    if (!od_span_valid(data) || data.n == 0u) {
        return 0u;
    }
    wd_clear();
    return od_display_write(stream_offset, data.p, data.n) ? (uint32_t)data.n : 0u;
}

od_mut_span_t od_xfer_app_inflate_scratch(void)
{
    return od_mut_span_make(s_inflate, sizeof s_inflate);
}

void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{
    (void)reason;
    od_display_abort();
}

/* The END acknowledgement is already in the link layer's FIFO (od_hal_radio_send pushes it
 * there); one more connection event puts it on air before the refresh holds the loop. */
od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
    (void)owner;
    (void)od_txq_process();
    bus_delay_ms(20u);
    return OD_XFER_BARRIER_PROCEED;
}

void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
    (void)owner;
    od_display_abort();
}

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
    if (completed == NULL || !od_display_active()) {
        return false;
    }
    *completed = od_display_refresh(mode);
    return true;
}

uint32_t od_xfer_app_now_ms(void)
{
    return od_hal_uptime_ms();
}
