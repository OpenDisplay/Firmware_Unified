/* od_hal_spi on nRF52840: bit-banged for the XIAO module's flash.
 *
 * The clock loop writes the port registers directly. Through the GPIO driver a 16 kB image takes
 * seconds on the loop thread, longer than a PIPE sender waits for its next ACK. Each phase still
 * needs settle() to be held: back-to-back port writes programmed corrupt data on the XIAO module
 * even though reads and the status register looked correct.
 *
 * Between transactions the bus is left driven (CS high, the rest low) so a sleeping flash never
 * sees floating inputs.
 */
#include "od_hal_spi.h"

#include "od_gpio.h"

#include <hal/nrf_gpio.h>
#include <stddef.h>
#include <zephyr/kernel.h>

static od_hal_spi_pins_t s_pins;
static bool s_open;
static bool s_in_txn;

typedef struct {
    NRF_GPIO_Type *port;
    uint32_t mask;
} fast_pin_t;

static fast_pin_t s_mosi;
static fast_pin_t s_miso;
static fast_pin_t s_sck;

static bool fast_pin(uint8_t cfg, fast_pin_t *out)
{
    uint8_t port;
    uint8_t pin;

    if (!od_pin_decode(cfg, &port, &pin)) {
        return false;
    }
    out->port = port == 0u ? NRF_P0 : NRF_P1;
    out->mask = 1uL << pin;
    return true;
}

static inline void settle(void)
{
    for (volatile uint32_t i = 0u; i < 8u; i++) {
    }
}

/* Mode 0, MSB first. */
static uint8_t shift(uint8_t out)
{
    uint8_t in = 0u;

    for (int bit = 7; bit >= 0; bit--) {
        if (((out >> bit) & 1u) != 0u) {
            nrf_gpio_port_out_set(s_mosi.port, s_mosi.mask);
        } else {
            nrf_gpio_port_out_clear(s_mosi.port, s_mosi.mask);
        }
        settle();
        nrf_gpio_port_out_set(s_sck.port, s_sck.mask);
        settle();
        in = (uint8_t)((in << 1) | ((nrf_gpio_port_in_read(s_miso.port) & s_miso.mask) != 0u));
        nrf_gpio_port_out_clear(s_sck.port, s_sck.mask);
        settle();
    }
    return in;
}

int od_hal_spi_open(const od_hal_spi_pins_t *pins)
{
    if (pins == NULL || pins->miso == 0xFFu || !fast_pin(pins->mosi, &s_mosi) ||
        !fast_pin(pins->miso, &s_miso) || !fast_pin(pins->sck, &s_sck)) {
        return OD_HAL_SPI_ENODEV;
    }
    s_pins = *pins;
    od_gpio_configure_output(s_pins.cs, true);
    od_gpio_configure_output(s_pins.sck, false);
    od_gpio_configure_output(s_pins.mosi, false);
    od_gpio_configure_input(s_pins.miso, false, false);
    s_open = true;
    s_in_txn = false;
    return OD_HAL_SPI_OK;
}

void od_hal_spi_close(void)
{
    if (!s_open) {
        return;
    }
    od_gpio_write(s_pins.cs, true);
    od_gpio_write(s_pins.sck, false);
    od_gpio_write(s_pins.mosi, false);
    od_gpio_configure_output(s_pins.miso, false);
    s_open = false;
    s_in_txn = false;
}

int od_hal_spi_xfer(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
    if (!s_open || s_in_txn) {
        return OD_HAL_SPI_EIO;
    }
    od_gpio_write(s_pins.cs, false);
    for (uint32_t i = 0u; i < tx_len; i++) {
        (void)shift(tx[i]);
    }
    for (uint32_t i = 0u; i < rx_len; i++) {
        rx[i] = shift(0x00u);
    }
    od_gpio_write(s_pins.cs, true);
    return OD_HAL_SPI_OK;
}

int od_hal_spi_begin(void)
{
    if (!s_open || s_in_txn) {
        return OD_HAL_SPI_EIO;
    }
    od_gpio_write(s_pins.cs, false);
    s_in_txn = true;
    return OD_HAL_SPI_OK;
}

int od_hal_spi_cont(const uint8_t *tx, uint32_t len)
{
    if (!s_in_txn) {
        return OD_HAL_SPI_EIO;
    }
    for (uint32_t i = 0u; i < len; i++) {
        (void)shift(tx[i]);
    }
    return OD_HAL_SPI_OK;
}

int od_hal_spi_end(void)
{
    if (!s_in_txn) {
        return OD_HAL_SPI_EIO;
    }
    od_gpio_write(s_pins.cs, true);
    s_in_txn = false;
    return OD_HAL_SPI_OK;
}
