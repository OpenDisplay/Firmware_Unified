/* See od_epd_nordic.h. */
#include "od_epd_nordic.h"

#include "od_epd_spi.h"
#include "od_gpio.h"
#include "od_hal_time.h"
#include "od_zephyr_compat.h"
#include "opendisplay_structs.h"

#include <stddef.h>

#define PIN_UNUSED 0xFFu

static uint8_t s_dc = PIN_UNUSED;
static uint8_t s_cs = PIN_UNUSED;
static uint8_t s_rst = PIN_UNUSED;
static uint8_t s_busy = PIN_UNUSED;

static void pin_write(uint8_t pin, bool high)
{
    if (pin != PIN_UNUSED) {
        od_gpio_write(pin, high);
    }
}

static void bus_cmd(uint8_t c)
{
    pin_write(s_dc, false);
    pin_write(s_cs, false);
    (void)od_epd_spi_write(&c, 1u);
    pin_write(s_cs, true);
    pin_write(s_dc, true);
}

static void bus_data(const uint8_t *d, size_t n)
{
    pin_write(s_cs, false);
    (void)od_epd_spi_write(d, n);
    pin_write(s_cs, true);
}

static void bus_reset(uint32_t hold_ms)
{
    if (s_rst == PIN_UNUSED) {
        return;
    }
    od_gpio_write(s_rst, true);
    od_msleep((int32_t)hold_ms);
    od_gpio_write(s_rst, false);
    od_msleep((int32_t)hold_ms);
    od_gpio_write(s_rst, true);
    od_msleep((int32_t)hold_ms);
}

static int bus_busy_read(void)
{
    return s_busy == PIN_UNUSED ? -1 : (od_gpio_read(s_busy) != 0 ? 1 : 0);
}

static void bus_delay_ms(uint32_t ms)
{
    od_msleep((int32_t)ms);
}

static const od_epd_cfg_bus_t s_bus = {
    .cmd = bus_cmd,
    .data = bus_data,
    .reset = bus_reset,
    .busy_read = bus_busy_read,
    .delay_ms = bus_delay_ms,
    .now_ms = od_hal_uptime_ms,
    .faulted = od_epd_spi_faulted,
};

const od_epd_cfg_bus_t *od_epd_nordic_bus(void)
{
    return &s_bus;
}

void od_epd_nordic_claim_pins(const struct DisplayConfig *d)
{
    const bool idle_high = od_epd_cfg_busy_idle_level() != 0u;

    s_dc = d->dc_pin;
    s_cs = d->cs_pin;
    s_rst = d->reset_pin;
    s_busy = d->busy_pin;
    if (s_dc != PIN_UNUSED) {
        od_gpio_configure_output(s_dc, true);
    }
    if (s_cs != PIN_UNUSED) {
        od_gpio_configure_output(s_cs, true);
    }
    if (s_rst != PIN_UNUSED) {
        od_gpio_configure_output(s_rst, true);
    }
    if (s_busy != PIN_UNUSED) {
        od_gpio_configure_input(s_busy, idle_high, !idle_high);
    }
}
