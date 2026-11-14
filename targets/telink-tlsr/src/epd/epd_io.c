/* The EPD_* primitives the imported Firmware_NRF52 drivers call, over tlsr_port.h GPIO.
 *
 * The nRF version drove a hardware SPI peripheral; here SPI is bit-banged because the pins come
 * from the config and the TLSR825x SPI master is tied to fixed pin groups. Mode 0, MSB first,
 * CS asserted for each transfer as the nRF driver's SS handling did, and the same 3-wire read:
 * MOSI turns around to an input for EPD_SPI_Read. Everything above the wire -- command/data
 * framing, FillRAM, the reset sequence, the busy-wait loop -- is the nRF file's logic unchanged,
 * except that the busy wait services the BLE stack instead of feeding a watchdog. */

#include "EPD_driver.h"

#include "tlsr_port.h"

#define BUFFER_SIZE 128
#define EPD_BUSY_CHECK_DELAY_MS 1

/* SystemConfig.pwr_pin has no polarity field. Every ATC_BLE_OEPL board read so far switches the
 * panel supply through an active-low enable (ATC reports enable_invert), so on this target the
 * pin is asserted LOW. A board with an active-high enable builds with OD_TLSR_PWR_ACTIVE_LOW=0;
 * the host cannot see which convention a device uses, so the config tool refuses a mismatch. */
#ifndef OD_TLSR_PWR_ACTIVE_LOW
#define OD_TLSR_PWR_ACTIVE_LOW 1
#endif
#define PWR_ON  (OD_TLSR_PWR_ACTIVE_LOW ? LOW : HIGH)
#define PWR_OFF (OD_TLSR_PWR_ACTIVE_LOW ? HIGH : LOW)

static struct epd_io_pins s_pins = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint8_t s_cs_mask = EPD_IO_CS_AUTO;
static uint16_t m_driver_refs = 0;
static bool s_mosi_is_input;
static bool s_busy_timed_out;

void epd_port_delay_ms(uint32_t ms)
{
    while (ms-- > 0u) {
        tlsr_port_delay_us(1000u);
        tlsr_port_service_stack();
    }
}

void epd_io_configure(const struct epd_io_pins *pins)
{
    s_pins = *pins;
}

void epd_io_cs_manual(uint8_t mask)
{
    s_cs_mask = mask;
    if (mask == EPD_IO_CS_AUTO) {
        tlsr_port_gpio_write(s_pins.cs, HIGH);
        tlsr_port_gpio_write(s_pins.cs2, HIGH);
        return;
    }
    tlsr_port_gpio_write(s_pins.cs, (mask & 0x01u) == 0u);
    tlsr_port_gpio_write(s_pins.cs2, (mask & 0x02u) == 0u);
}

/* Per-transfer framing only in auto mode; in manual mode the lines stay as set. */
static void cs_frame(bool assert)
{
    if (s_cs_mask == EPD_IO_CS_AUTO) {
        tlsr_port_gpio_write(s_pins.cs, !assert);
    }
}

void epd_io_park_power(uint8_t pin)
{
    if (pin != TLSR_PORT_PIN_NONE && m_driver_refs == 0) {
        tlsr_port_gpio_output(pin, PWR_OFF);
    }
}

bool epd_io_busy_timed_out(void)
{
    return s_busy_timed_out;
}

void EPD_GPIO_Init(void)
{
    if (m_driver_refs++ > 0) return;
    if (s_pins.pwr != TLSR_PORT_PIN_NONE) {
        tlsr_port_gpio_output(s_pins.pwr, PWR_ON);
    }
    if (s_pins.pwr2 != TLSR_PORT_PIN_NONE) {
        tlsr_port_gpio_output(s_pins.pwr2, PWR_ON);
    }
    s_cs_mask = EPD_IO_CS_AUTO;
    tlsr_port_gpio_output(s_pins.dc, LOW);
    tlsr_port_gpio_output(s_pins.rst, HIGH);
    tlsr_port_gpio_output(s_pins.cs, HIGH);
    tlsr_port_gpio_output(s_pins.cs2, HIGH);
    tlsr_port_gpio_output(s_pins.sclk, LOW);
    tlsr_port_gpio_output(s_pins.mosi, LOW);
    tlsr_port_gpio_input(s_pins.busy, TLSR_PORT_PULL_NONE);
    s_mosi_is_input = false;
}

void EPD_GPIO_Uninit(void)
{
    if (m_driver_refs == 0) return;
    if (--m_driver_refs > 0) return;
    tlsr_port_gpio_write(s_pins.dc, LOW);
    tlsr_port_gpio_write(s_pins.cs, LOW);
    tlsr_port_gpio_write(s_pins.cs2, LOW);
    tlsr_port_gpio_write(s_pins.rst, LOW);
    /* Held at the off level, not released: a floating enable can half-open an active-low
     * switch and leak through the panel. */
    if (s_pins.pwr != TLSR_PORT_PIN_NONE) tlsr_port_gpio_write(s_pins.pwr, PWR_OFF);
    if (s_pins.pwr2 != TLSR_PORT_PIN_NONE) tlsr_port_gpio_write(s_pins.pwr2, PWR_OFF);
    s_cs_mask = EPD_IO_CS_AUTO;
    tlsr_port_gpio_release(s_pins.mosi);
    tlsr_port_gpio_release(s_pins.sclk);
    tlsr_port_gpio_release(s_pins.cs);
    tlsr_port_gpio_release(s_pins.cs2);
    tlsr_port_gpio_release(s_pins.dc);
    tlsr_port_gpio_release(s_pins.rst);
    tlsr_port_gpio_release(s_pins.busy);
}

void EPD_SPI_Write(uint8_t* value, uint8_t len)
{
    if (s_mosi_is_input) {
        tlsr_port_gpio_output(s_pins.mosi, LOW);
        s_mosi_is_input = false;
    }
    cs_frame(true);
    tlsr_port_spi_out(s_pins.mosi, s_pins.sclk, value, len);
    cs_frame(false);
}

void EPD_SPI_Read(uint8_t* value, uint8_t len)
{
    uint8_t i, bit, b;

    if (!s_mosi_is_input) {
        tlsr_port_gpio_input(s_pins.mosi, TLSR_PORT_PULL_NONE);
        s_mosi_is_input = true;
    }
    cs_frame(true);
    for (i = 0; i < len; i++) {
        b = 0u;
        for (bit = 0x80u; bit != 0u; bit >>= 1) {
            tlsr_port_gpio_write(s_pins.sclk, HIGH);
            if (tlsr_port_gpio_read(s_pins.mosi)) b |= bit;
            tlsr_port_gpio_write(s_pins.sclk, LOW);
        }
        value[i] = b;
    }
    cs_frame(false);
}

void EPD_WriteCmd(uint8_t cmd) {
    tlsr_port_gpio_write(s_pins.dc, LOW);
    EPD_SPI_Write(&cmd, 1);
}

void EPD_WriteData(uint8_t* value, uint8_t len) {
    tlsr_port_gpio_write(s_pins.dc, HIGH);
    EPD_SPI_Write(value, len);
}

void EPD_ReadData(uint8_t* value, uint8_t len) {
    tlsr_port_gpio_write(s_pins.dc, HIGH);
    EPD_SPI_Read(value, len);
}

void EPD_WriteByte(uint8_t value) {
    tlsr_port_gpio_write(s_pins.dc, HIGH);
    EPD_SPI_Write(&value, 1);
}

uint8_t EPD_ReadByte(void) {
    uint8_t value;
    tlsr_port_gpio_write(s_pins.dc, HIGH);
    EPD_SPI_Read(&value, 1);
    return value;
}

void EPD_FillRAM(uint8_t cmd, uint8_t value, uint32_t len) {
    uint8_t buffer[BUFFER_SIZE];
    for (uint8_t i = 0; i < BUFFER_SIZE; i++) buffer[i] = value;
    EPD_WriteCmd(cmd);
    uint32_t remaining = len;
    while (remaining > 0) {
        uint16_t chunk_size = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        EPD_WriteData(buffer, chunk_size);
        remaining -= chunk_size;
    }
}

void EPD_Reset(bool status, uint16_t duration) {
    tlsr_port_gpio_write(s_pins.rst, status);
    delay(duration);
    tlsr_port_gpio_write(s_pins.rst, status ? LOW : HIGH);
    delay(duration);
    tlsr_port_gpio_write(s_pins.rst, status);
    delay(duration);
}

bool EPD_ReadBusy(void) { return tlsr_port_gpio_read(s_pins.busy); }

void EPD_WaitBusy(bool status, uint16_t timeout) {
    s_busy_timed_out = false;
    while (EPD_ReadBusy() == status) {
        delay(EPD_BUSY_CHECK_DELAY_MS);
        timeout--;
        if (timeout == 0) {
            s_busy_timed_out = true;
            break;
        }
    }
}

uint16_t EPD_ReadVoltage(void) {
    return 0;   /* no ADC path on this target yet */
}
