/* See od_epd_nordic.h. */
#include "od_epd_nordic.h"

#include "od_claim_mode.h"
#include "od_epd_spi.h"
#include "od_gpio.h"
#include "od_hal_time.h"
#include "od_zephyr_compat.h"
#include "opendisplay_structs.h"

#include <stddef.h>

#include <zephyr/kernel.h>

#define PIN_UNUSED 0xFFu

static uint8_t s_dc = PIN_UNUSED;
static uint8_t s_cs = PIN_UNUSED;     /* the CS the next cmd/data frames */
static uint8_t s_cs_pins[2] = { PIN_UNUSED, PIN_UNUSED };
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

/* The thread that owns od_claim_mode (main.c's superloop). Set once, before the radio. */
static k_tid_t s_loop_tid;

void od_epd_nordic_bind_loop_thread(void)
{
    s_loop_tid = k_current_get();
}

/* Every BUSY poll and fixed refresh delay of a pass ends up here. A setup screen is drawn ON the
 * loop thread (od_claim_mode_tick -> od_claim_app_draw) and blocks it for ~20 s, past T_ARM and
 * T_CLEAR: without a count tick in that wait, a claim-mode boot (k = 1) never writes k + 1, so the
 * five-cycle factory reset of an unclaimed device stalls at k = 1 forever (observed on EN04
 * OD114608). Same remedy as ESP32 / Silabs: 100 ms slices, each followed by the writes-only
 * od_claim_mode_tick_count(). od_claim_mode is not thread-safe, so the tick runs only when the
 * caller IS the loop thread; the boot-screen work queue already has its own ticks (opendisplay_ble.c). */
static void bus_delay_ms(uint32_t ms)
{
    const bool tick = s_loop_tid != NULL && k_current_get() == s_loop_tid;

    while (ms > 0u) {
        const uint32_t step = ms > 100u ? 100u : ms;

        od_msleep((int32_t)step);
        ms -= step;
        if (tick) {
            od_claim_mode_tick_count();
        }
    }
}

static bool bus_pin_write(uint8_t pin, uint8_t level)
{
    if (pin == PIN_UNUSED || level > 2u) {
        return false;
    }
    if (level == 2u) {
        od_gpio_configure_input(pin, false, false);
    } else {
        od_gpio_configure_output(pin, level != 0u);
    }
    return true;
}

/* Dual-controller panels: od_epd only offers CS_SELECT when cs_pin_2 is a distinct pin. */
static bool bus_cs_select(uint8_t which)
{
    if (which > 1u || s_cs_pins[which] == PIN_UNUSED) {
        return false;
    }
    s_cs = s_cs_pins[which];
    return true;
}

/* read_bytes stays NULL: the Nordic panel bus is write-only (MOSI/SCK through nrfx), so
 * READ_SCRATCH is refused at validation rather than returning garbage. */
static const od_epd_cfg_bus_t s_bus = {
    .cmd = bus_cmd,
    .data = bus_data,
    .reset = bus_reset,
    .busy_read = bus_busy_read,
    .delay_ms = bus_delay_ms,
    .now_ms = od_hal_uptime_ms,
    .faulted = od_epd_spi_faulted,
    .pin_write = bus_pin_write,
    .cs_select = bus_cs_select,
};

const od_epd_cfg_bus_t *od_epd_nordic_bus(void)
{
    return &s_bus;
}

void od_epd_nordic_claim_pins(const struct DisplayConfig *d)
{
    const bool idle_high = od_epd_cfg_busy_idle_level() != 0u;

    s_dc = d->dc_pin;
    s_cs_pins[0] = d->cs_pin;
    s_cs_pins[1] = d->cs_pin_2 != d->cs_pin ? d->cs_pin_2 : PIN_UNUSED;
    s_cs = s_cs_pins[0];
    s_rst = d->reset_pin;
    s_busy = d->busy_pin;
    if (s_dc != PIN_UNUSED) {
        od_gpio_configure_output(s_dc, true);
    }
    for (unsigned i = 0u; i < 2u; i++) {
        if (s_cs_pins[i] != PIN_UNUSED) {
            od_gpio_configure_output(s_cs_pins[i], true);
        }
    }
    if (s_rst != PIN_UNUSED) {
        od_gpio_configure_output(s_rst, true);
    }
    if (s_busy != PIN_UNUSED) {
        od_gpio_configure_input(s_busy, idle_high, !idle_high);
    }
}
