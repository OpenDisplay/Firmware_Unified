/* od_hal_spi on EFR32BG22 -- bit-banged.
 *
 * BIT-BANGED RATHER THAN USART, deliberately. The panel already owns a USART on this board and
 * is driven from a config-supplied sequence that can assert CS at times this module cannot
 * predict. Sharing a peripheral between the two would make a flash failure depend on panel
 * timing, which is the hardest class of bug to pin down on a device with no debugger attached.
 * The flash moves at most a 256-byte page at a time and never during a refresh, so the clock
 * rate buys nothing worth that risk.
 */
#include "od_hal_spi.h"

#include <stddef.h>

#include "em_cmu.h"
#include "em_gpio.h"
#include "sl_udelay.h"

#define PIN_UNUSED 0xFFu

static struct {
    GPIO_Port_TypeDef mosi_p, miso_p, sck_p, cs_p;
    uint8_t mosi, miso, sck, cs;
    bool open;
    bool in_txn;
} s;

static bool decode(uint8_t v, GPIO_Port_TypeDef *port, uint8_t *pin)
{
    if (v == PIN_UNUSED) {
        return false;
    }
    const unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
    const unsigned pn = (unsigned)(v & 0x0Fu);
    if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
        return false;
    }
    *port = (GPIO_Port_TypeDef)(gpioPortA + pr);
    *pin = (uint8_t)pn;
    return true;
}

/* SPI mode 0: MOSI set while SCK is low, MISO sampled on the rising edge. */
static uint8_t shift(uint8_t out)
{
    uint8_t in = 0u;

    for (int i = 7; i >= 0; i--) {
        if ((out >> i) & 1u) {
            GPIO_PinOutSet(s.mosi_p, s.mosi);
        } else {
            GPIO_PinOutClear(s.mosi_p, s.mosi);
        }
        sl_udelay_wait(1u);
        GPIO_PinOutSet(s.sck_p, s.sck);
        in = (uint8_t)((in << 1) | (GPIO_PinInGet(s.miso_p, s.miso) ? 1u : 0u));
        sl_udelay_wait(1u);
        GPIO_PinOutClear(s.sck_p, s.sck);
    }
    return in;
}

int od_hal_spi_open(const od_hal_spi_pins_t *p)
{
    if (p == NULL) {
        return OD_HAL_SPI_ENODEV;
    }
    if (!decode(p->mosi, &s.mosi_p, &s.mosi) ||
        !decode(p->sck, &s.sck_p, &s.sck) ||
        !decode(p->cs, &s.cs_p, &s.cs) ||
        !decode(p->miso, &s.miso_p, &s.miso)) {
        /* MISO included: a bus that cannot read is not a bus this HAL will open. */
        s.open = false;
        return OD_HAL_SPI_ENODEV;
    }

    CMU_ClockEnable(cmuClock_GPIO, true);
    GPIO_PinModeSet(s.cs_p, s.cs, gpioModePushPull, 1);
    GPIO_PinModeSet(s.sck_p, s.sck, gpioModePushPull, 0);
    GPIO_PinModeSet(s.mosi_p, s.mosi, gpioModePushPull, 0);
    GPIO_PinModeSet(s.miso_p, s.miso, gpioModeInput, 0);
    s.open = true;
    s.in_txn = false;
    return OD_HAL_SPI_OK;
}

void od_hal_spi_close(void)
{
    if (!s.open) {
        return;
    }
    GPIO_PinOutSet(s.cs_p, s.cs);          /* deselect before letting the pins go */
    GPIO_PinModeSet(s.sck_p, s.sck, gpioModeDisabled, 0);
    GPIO_PinModeSet(s.mosi_p, s.mosi, gpioModeDisabled, 0);
    GPIO_PinModeSet(s.miso_p, s.miso, gpioModeDisabled, 0);
    s.open = false;
    s.in_txn = false;
}

int od_hal_spi_xfer(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len)
{
    if (!s.open || s.in_txn) {
        return OD_HAL_SPI_EIO;
    }
    GPIO_PinOutClear(s.cs_p, s.cs);
    for (uint32_t i = 0; i < tx_len; i++) {
        (void)shift(tx[i]);
    }
    for (uint32_t i = 0; i < rx_len; i++) {
        rx[i] = shift(0x00u);
    }
    GPIO_PinOutSet(s.cs_p, s.cs);
    return OD_HAL_SPI_OK;
}

int od_hal_spi_begin(void)
{
    if (!s.open || s.in_txn) {
        return OD_HAL_SPI_EIO;
    }
    GPIO_PinOutClear(s.cs_p, s.cs);
    s.in_txn = true;
    return OD_HAL_SPI_OK;
}

int od_hal_spi_cont(const uint8_t *tx, uint32_t len)
{
    if (!s.in_txn) {
        return OD_HAL_SPI_EIO;
    }
    for (uint32_t i = 0; i < len; i++) {
        (void)shift(tx[i]);
    }
    return OD_HAL_SPI_OK;
}

int od_hal_spi_end(void)
{
    if (!s.in_txn) {
        return OD_HAL_SPI_EIO;
    }
    GPIO_PinOutSet(s.cs_p, s.cs);
    s.in_txn = false;
    return OD_HAL_SPI_OK;
}
