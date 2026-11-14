/* The seam between the two halves of this target.
 *
 * SDK side (tlsr_port.c, app*.c, main.c) is compiled with the Telink SDK's flags, which include
 * -fpack-struct and -fshort-enums because the prebuilt BLE library was built with them. The OD
 * side (od_*.c and all of shared/) is compiled without them. A struct or enum crossing between
 * the two would silently change layout or size, so this header carries plain integers and byte
 * pointers only, and neither side includes the other's headers. */
#ifndef TLSR_PORT_H
#define TLSR_PORT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- implemented on the SDK side (tlsr_port.c) ---- */

#define TLSR_PORT_TICKS_PER_MS 16000u     /* 16 MHz system timer, 32 bits: wraps every ~268 s */
uint32_t tlsr_port_ticks(void);
void     tlsr_port_delay_us(uint32_t us);

void tlsr_port_flash_read(uint32_t addr, uint32_t len, uint8_t *buf);
void tlsr_port_flash_program(uint32_t addr, uint32_t len, const uint8_t *buf);  /* within one 256 B page */
void tlsr_port_flash_erase_4k(uint32_t addr);

#define TLSR_PORT_NOTIFY_SENT   0
#define TLSR_PORT_NOTIFY_BUSY   1         /* TX FIFO full: offer the same frame again */
#define TLSR_PORT_NOTIFY_FAILED 2         /* not connected, not subscribed or too long for the MTU */
int  tlsr_port_notify(const uint8_t *frame, uint16_t len);
bool tlsr_port_connected(void);
void tlsr_port_disconnect(void);

/* One raw AES-128 block on the hardware engine, interrupts masked for its duration because the
 * BLE link layer drives the same engine from its ISR. The engine's byte order is established at
 * boot by od_hal_crypto.c's known-answer test, not assumed here. */
void tlsr_port_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
uint32_t tlsr_port_noise32(void);         /* SDK rand(): analog-noise-seeded, NOT a CSPRNG */

/* GPIO by OpenDisplay pin number: bits 7..3 select the port (0 = PA ... 4 = PE), bits 2..0 the
 * pin, so PA0 = 0, PB4 = 12, PD7 = 31. 0xFF is "not connected" and every call ignores it. */
#define TLSR_PORT_PIN_NONE      0xFFu
#define TLSR_PORT_PULL_NONE     0u
#define TLSR_PORT_PULL_UP       1u
#define TLSR_PORT_PULL_DOWN     2u
void tlsr_port_gpio_output(uint8_t pin, bool level);
void tlsr_port_gpio_input(uint8_t pin, uint8_t pull);
void tlsr_port_gpio_write(uint8_t pin, bool level);
bool tlsr_port_gpio_read(uint8_t pin);
void tlsr_port_gpio_release(uint8_t pin);   /* input, floating: the lowest-leakage idle state */

/* Bit-banged SPI out, mode 0, MSB first, `n` bytes: data set with the clock low, sampled on the
 * rising edge. Both pins must already be outputs; chip-select is the caller's. Runs from RAM and
 * writes the output registers directly -- when both pins share a port, two plain stores per
 * bit. The TLSR825x SPI master only exists on fixed pin groups the ATC boards do not use. */
void tlsr_port_spi_out(uint8_t mosi, uint8_t sclk, const uint8_t *buf, uint32_t n);

/* An LED pin between flashes. PA7 is also SWS, the wired-flashing line (and the blue LED on ATC
 * boards): it goes back to its SWS function with the default 1 MOhm pull-up, so a tag stays
 * flashable over the wire whenever no pattern is running. Every other pin is released. */
#define TLSR_PORT_PIN_SWS       7u
void tlsr_port_led_park(uint8_t pin);

/* One pass of the BLE stack's main-loop work, for code that must block for a long time (a panel
 * refresh). Safe from inside od_tlsr_poll(): the only callbacks it can raise are the ATT write
 * (which only queues into od_rxq) and link up/down (which only set flags). */
void tlsr_port_service_stack(void);

/* Keep the MCU out of suspend while the panel is driven or an LED pattern runs. The stack's main
 * loop otherwise sleeps until the next radio event -- up to an advertising interval (500 ms) per
 * pass, turning a 1 ms busy-wait step, one rendered row or a 100 ms LED step into half a second.
 * One bit per holder, so neither can release the other's hold. */
#define TLSR_PORT_AWAKE_PANEL   0x01u
#define TLSR_PORT_AWAKE_LED     0x02u
void tlsr_port_stay_awake(uint8_t holder, bool on);

/* Breadcrumbs for field debugging without a log: the current step of the frame path lives in an
 * analog register that a watchdog reset does not clear (only a power cycle does), so after a hang
 * the rebooted firmware can advertise where the previous run died. 0 = idle. */
void tlsr_port_crumb(uint8_t step);
/* Called once at boot: returns the step the previous run died in (0 = it did not) and bumps the
 * persistent reset counter when it did. *resets receives that counter. */
uint8_t tlsr_port_crumb_boot(uint8_t *resets);

/* Allow the Telink OTA service to accept an image on the current connection (ENTER_DFU); the
 * link going down disarms it. */
void tlsr_port_ota_arm(bool on);

/* Supply voltage in mV, Telink's way on the 825x: the ADC pin is driven high (so it sits at the
 * supply) and measured through the 1/8 prescaler with the chip's factory calibration, then
 * released. Only PB0..PB7, PC4 and PC5 reach the ADC; any other pin returns 0. ATC tags use PB3. */
uint16_t tlsr_port_battery_mv(uint8_t pin);

void tlsr_port_mac(uint8_t out[6]);
void tlsr_port_set_adv_msd(const uint8_t msd[16]);
void tlsr_port_reboot(void);

/* ---- implemented on the OD side (od_tlsr_app.c), called from the SDK side ---- */

void od_tlsr_init(void);
void od_tlsr_poll(void);
void od_tlsr_on_connect(void);
void od_tlsr_on_disconnect(void);
void od_tlsr_on_write(const uint8_t *data, uint16_t len);
void od_tlsr_on_notify_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif /* TLSR_PORT_H */
