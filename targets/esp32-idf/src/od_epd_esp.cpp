/* See od_epd_esp.h. */
#include "od_claim_mode.h"
#include "od_epd_esp.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#include "od_hal_gpio.h"
#include "od_hal_sleep.h"
#include "od_hal_time.h"
#include "od_log.h"
#include "opendisplay_structs.h"

#define PIN_UNUSED 0xFFu
/* The rate every SPI panel on this target has been driven at. */
#define PANEL_SPI_HZ 8000000
/* One constant for the bus's max_transfer_sz and the chunk size below. */
#define PANEL_MAX_XFER 4096

/* SPI2 is the general-purpose host on every variant this target builds (SPI1 is flash). */
#define PANEL_SPI_HOST SPI2_HOST

static spi_device_handle_t s_dev;
static bool s_bus_up;
static bool s_fault;

static uint8_t s_dc = PIN_UNUSED;
static uint8_t s_cs = PIN_UNUSED;   /* the CS the next cmd/data frames */
static uint8_t s_cs_pins[2] = { PIN_UNUSED, PIN_UNUSED };
static uint8_t s_rst = PIN_UNUSED;
static uint8_t s_busy = PIN_UNUSED;

static void pin_level(uint8_t pin, bool high)
{
    if (pin != PIN_UNUSED) {
        od_hal_gpio_write(pin, high);
    }
}

static void xfer(const uint8_t *d, size_t n)
{
    if (!s_bus_up || d == nullptr) {
        s_fault = true;
        return;
    }
    while (n > 0u) {
        const size_t l = n > (size_t)PANEL_MAX_XFER ? (size_t)PANEL_MAX_XFER : n;
        spi_transaction_t t = {};

        t.length = l * 8u;
        if (l <= 4u) {
            t.flags = SPI_TRANS_USE_TXDATA;
            memcpy(t.tx_data, d, l);
        } else {
            t.tx_buffer = d;
        }
        const esp_err_t rc = spi_device_polling_transmit(s_dev, &t);
        if (rc != ESP_OK) {
            if (!s_fault) {
                od_log_error("panel: SPI transmit failed (%s)", esp_err_to_name(rc));
            }
            s_fault = true;
            return;
        }
        d += l;
        n -= l;
    }
}

static void bus_cmd(uint8_t c)
{
    pin_level(s_dc, false);
    pin_level(s_cs, false);
    xfer(&c, 1u);
    pin_level(s_cs, true);
    pin_level(s_dc, true);
}

static void bus_data(const uint8_t *d, size_t n)
{
    if (n == 0u) {
        return;
    }
    pin_level(s_cs, false);
    xfer(d, n);
    pin_level(s_cs, true);
}

static void bus_reset(uint32_t hold_ms)
{
    if (s_rst == PIN_UNUSED) {
        return;
    }
    od_hal_gpio_write(s_rst, true);
    od_hal_delay_ms(hold_ms);
    od_hal_gpio_write(s_rst, false);
    od_hal_delay_ms(hold_ms);
    od_hal_gpio_write(s_rst, true);
    od_hal_delay_ms(hold_ms);
}

static int bus_busy_read(void)
{
    return s_busy == PIN_UNUSED ? -1 : (od_hal_gpio_read(s_busy) != 0 ? 1 : 0);
}

static void bus_delay_ms(uint32_t ms)
{
    od_hal_delay_ms(ms);
    /* The refresh waits run on the loop task and can outlast T_ARM: keep the boot count on
     * time (od_claim_mode.h, SECURITY_V2 7.2). */
    od_claim_mode_tick_count();
}

static bool bus_faulted(void)
{
    return s_fault;
}

/* SET_PIN: the pin arrives resolved from the DisplayConfig. Level 2 releases it to input. */
static bool bus_pin_write(uint8_t pin, uint8_t level)
{
    if (pin == PIN_UNUSED || level > 2u || !od_hal_gpio_pin_valid(pin)) {
        return false;
    }
    if (level == 2u) {
        od_hal_gpio_config_input(pin, false, false);
    } else {
        od_hal_gpio_config_output(pin, level != 0u);
    }
    return true;
}

/* Dual-controller panels: od_epd_cfg offers CS_SELECT only when cs_pin_2 is a distinct pin. */
static bool bus_cs_select(uint8_t which)
{
    if (which > 1u || s_cs_pins[which] == PIN_UNUSED) {
        return false;
    }
    s_cs = s_cs_pins[which];
    return true;
}

/* read_bytes stays NULL: the panel SPI device is write-only (no MISO), so READ_SCRATCH is refused
 * at validation rather than returning garbage. */
static const od_epd_cfg_bus_t s_bus = {
    .cmd = bus_cmd,
    .data = bus_data,
    .reset = bus_reset,
    .busy_read = bus_busy_read,
    .delay_ms = bus_delay_ms,
    .now_ms = od_hal_uptime_ms,
    .faulted = bus_faulted,
    .pin_write = bus_pin_write,
    .cs_select = bus_cs_select,
    .read_bytes = nullptr,
};

const od_epd_cfg_bus_t *od_epd_esp_bus(void)
{
    return &s_bus;
}

void od_epd_esp_close(void)
{
    if (s_dev != nullptr) {
        spi_bus_remove_device(s_dev);
        s_dev = nullptr;
    }
    if (s_bus_up) {
        const esp_err_t rc = spi_bus_free(PANEL_SPI_HOST);

        if (rc != ESP_OK) {
            od_log_warn("panel: spi_bus_free failed (%s)", esp_err_to_name(rc));
        }
    }
    s_bus_up = false;
}

bool od_epd_esp_is_open(void)
{
    return s_bus_up;
}

bool od_epd_esp_faulted(void)
{
    return s_fault;
}

static void claim_output(uint8_t pin, bool high)
{
    if (pin != PIN_UNUSED && od_hal_gpio_pin_valid(pin)) {
        od_hal_gpio_config_output(pin, high);
    }
}

bool od_epd_esp_open(const struct DisplayConfig *d)
{
    if (d == nullptr) {
        return false;
    }
    od_epd_esp_close();
    s_fault = false;

    s_dc = d->dc_pin;
    s_cs_pins[0] = d->cs_pin;
    s_cs_pins[1] = (d->cs_pin_2 != d->cs_pin) ? d->cs_pin_2 : PIN_UNUSED;
    s_cs = s_cs_pins[0];
    s_rst = d->reset_pin;
    s_busy = d->busy_pin;

    claim_output(s_dc, true);
    claim_output(s_cs_pins[0], true);
    claim_output(s_cs_pins[1], true);
    claim_output(s_rst, true);
    if (s_busy != PIN_UNUSED && od_hal_gpio_pin_valid(s_busy)) {
        const bool idle_high = od_epd_cfg_busy_idle_level() != 0u;

        od_hal_gpio_config_input(s_busy, idle_high, !idle_high);
    }

    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = d->data_pin == PIN_UNUSED ? -1 : (int)d->data_pin;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = d->clk_pin == PIN_UNUSED ? -1 : (int)d->clk_pin;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = PANEL_MAX_XFER;
    esp_err_t rc = spi_bus_initialize(PANEL_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (rc != ESP_OK) {
        od_log_error("panel: spi_bus_initialize failed (%s)", esp_err_to_name(rc));
        return false;
    }
    s_bus_up = true;

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = PANEL_SPI_HZ;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;   /* CS is a GPIO, framed per cmd/data above */
    devcfg.queue_size = 1;
    devcfg.flags = SPI_DEVICE_HALFDUPLEX;
    rc = spi_bus_add_device(PANEL_SPI_HOST, &devcfg, &s_dev);
    if (rc != ESP_OK) {
        od_log_error("panel: spi_bus_add_device failed (%s)", esp_err_to_name(rc));
        s_dev = nullptr;
        od_epd_esp_close();
        return false;
    }
    return true;
}
